/* -*- coding: utf-8 -*- */
/* -*- mode: c -*- */
/*
 * Dislocker -- enables to read/write on BitLocker encrypted partitions under
 * Linux
 * Copyright (C) 2012  Romain Coltel, Hervé Schauer Consultants
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */


#include <errno.h>

#include "recovery_password.h"
#include "polarssl/sha2.h"
#include "ssl_bindings.h"


/**
 * Function implementing the algorithm of the chain hash, described by Jesse D. Kornblum.
 * Ref: http://jessekornblum.com/presentations/di09.pdf
 * 
 * @param recovery_key The 16-bytes recovery key previously distilled (16 bytes)
 * @param salt The salt used for crypto (16 bytes)
 * @param result Will contain the resulting hash key (32 bytes)
 * @return TRUE if result can be trusted, FALSE otherwise
 */
int chain_hash(const uint8_t *recovery_key,
			   const uint8_t *salt,
			   uint8_t *result)
{
	size_t size = sizeof(bitlocker_chain_hash_t);
	bitlocker_chain_hash_t * ch = NULL;
	uint64_t loop = 0;
	
	ch = (bitlocker_chain_hash_t *) xmalloc(size);
	
	memset(ch, 0, size);
	
	/* 16 is the size of the recovery_key, in bytes (see doc above) */
	SHA256(recovery_key, 16, ch->password_hash);
	
	memcpy(ch->salt, salt, SALT_LENGTH);
	
	for(loop = 0; loop < 0x100000; ++loop)
	{
		SHA256((unsigned char *)ch, size, ch->updated_hash);
		
		ch->hash_count++;
	}
	
	memcpy(result, ch->updated_hash, SHA256_DIGEST_LENGTH);
	
	/* Wipe out with zeros and free it */
	memclean(ch, size);
	
	return TRUE;
} // End chain_hash


/**
 * Validating one block of the recovery password
 * 
 * @param digits The entire block to validate
 * @param block_nb The block number
 * @param short_password The block divided by 11 and converted into uint16_t
 * @return TRUE if valid, FALSE otherwise
 */
int valid_block(uint8_t* digits, int block_nb, uint16_t* short_password)
{
	// Check the parameters
	if(!digits)
		return FALSE;
	
	
	/* Convert chars into int */
	errno = 0;
	long int block = strtol((char *)digits, (char **) NULL, 10);
	if(errno == ERANGE)
	{
		xprintf(L_ERROR, "Error converting '%s' into a number: errno=ERANGE", digits);
		return FALSE;
	}
	
	/* 1st check --  Checking if the bloc is divisible by eleven */
	if((block % 11) != 0)
	{
		xprintf(L_ERROR, "Error handling the recovery password: Bloc n°%d (%d) invalid. "
		"It has to be divisible by 11.\n", block_nb, block);
		return FALSE;
	}
	
	/* 2nd check -- Checking if the bloc is less than 2**16 * 11 */
	if(block >= 720896)
	{
		xprintf(L_ERROR,  "Error handling the recovery password: Bloc n°%d (%d) invalid. "
		"It has to be less than 2**16 * 11 (720896).\n", block_nb, block);
		return FALSE;
	}
	
	/* 3rd check -- Checking if the checksum is correct */
	int8_t check_digit = (int8_t)(digits[0] - digits[1] + digits[2] - digits[3] + digits[4]
									- 48) % 11; /* Convert chars into digits */
	
	/* Some kind of bug the c modulo has: -2 % 11 yields -2 instead of 9 */
	while(check_digit < 0)
		check_digit = (int8_t)(check_digit + 11);
	
	if(check_digit != (digits[5] - 48))
	{
		xprintf(L_ERROR, "Error handling the recovery password: Bloc n°%d (%d) invalid.\n", block_nb, block);
		return FALSE;
	}
	
	/*
	 * The bloc has a good look, store a short version of it
	 * We already have checked the size (see 2nd check), a uint16_t can contain
	 * the result
	 */
	if(short_password)
		*short_password = (uint16_t) (block / 11);
	
	return TRUE;
}


/**
 * Validating (or not) the recovery password
 * If the password is valid, 
 * 
 * @param recovery_password The recovery password the user typed (48+7 bytes)
 * @param short_password The recovery password converted in uint16_t (8 uint16_t)
 * @return TRUE if valid, FALSE otherwise
 */
int is_valid_key(const uint8_t *recovery_password, uint16_t *short_password)
{
	// Check the parameters
	if(recovery_password == NULL)
		return FALSE;
	
	if(short_password == NULL)
		return FALSE;
	
	/* Begin by checking the length of the password */
	if(strlen((char*)recovery_password) != 48+7)
	{
		xprintf(L_ERROR, "Error handling the recovery password: Wrong length (Has to be %d)\n", 48+7);
		return FALSE;
	}
	
	const uint8_t *rp = recovery_password;
	uint16_t *sp = short_password;
	uint8_t digits[NB_DIGIT_BLOC + 1];
	
	int loop = 0;
	
	for(loop = 0; loop < NB_RP_BLOCS; ++loop)
	{
		memcpy(digits, rp, NB_DIGIT_BLOC);
		digits[NB_DIGIT_BLOC] = 0;
		
		/* Check block validity */
		if(!valid_block(digits, loop+1, sp))
			return FALSE;
		
		sp++;
		rp += 7;
	}
	
	// All of the recovery password seems to be good
	
	return TRUE;
} // End is_valid_key



/**
 * Calculate the intermediate key from a raw recovery password
 * 
 * @param recovery_password The raw recovery password given by the user (48+7 bytes)
 * @param result_key The intermediate key used to decrypt the associated VMK (32 bytes)
 * @return TRUE if result_key can be trusted, FALSE otherwise
 */
int intermediate_key(const uint8_t *recovery_password,
					 const uint8_t *salt,
					 uint8_t *result_key)
{
	// Check the parameters
	if(recovery_password == NULL)
	{
		xprintf(L_ERROR, "Error: No recovery password given, aborting calculation of the intermediate key.\n");
		return FALSE;
	}
	
	if(result_key == NULL)
	{
		xprintf(L_ERROR, "Error: No space to store the intermediate recovery key, aborting operation.\n");
		return FALSE;
	}
	
	
	uint16_t passwd[NB_RP_BLOCS];
	uint8_t *iresult = xmalloc(INTERMEDIATE_KEY_LENGTH * sizeof(uint8_t));
	uint8_t *iresult_save = iresult;
	int loop = 0;
	
	memset(passwd,  0, NB_RP_BLOCS * sizeof(uint16_t));
	memset(iresult, 0, INTERMEDIATE_KEY_LENGTH * sizeof(uint8_t));
	
	/* Check if the recovery_password has a good smile */
	if(!is_valid_key(recovery_password, passwd))
	{
		xfree(iresult);
		return FALSE;
	}
	
	// passwd now contains the blocs divided by 11 in a uint16_t tab
	// Convert each one of the blocs in little endian and make it one buffer
	for(loop = 0; loop < NB_RP_BLOCS; ++loop)
	{
		*iresult = (uint8_t)(passwd[loop] & 0x00ff);
		iresult++;
		*iresult = (uint8_t)((passwd[loop] & 0xff00) >> 8);
		iresult++;
	}
	
	iresult = iresult_save;
	
	/* Just print it */
	char s[NB_RP_BLOCS*2 * 5 + 1] = {0,};
	for (loop = 0; loop < NB_RP_BLOCS*2; ++loop)
		snprintf(&s[loop*5], 6, "0x%02x ", iresult[loop]);
	
	xprintf(L_INFO, "Distilled password: '%s\b'\n", s);
	
	chain_hash(iresult, salt, result_key);
	
	xfree(iresult);
	
	/* We successfuly retrieve the key! */
	return TRUE;
} // End intermediate_key


/**
 * Prompt for the recovery password to be entered
 * 
 * @param rp The place where to put the entered recovery password
 * @return TRUE if result_key can be trusted, FALSE otherwise
 */
int prompt_rp(uint8_t** rp)
{
	// Check the parameter
	if(!rp)
		return FALSE;
	
	
	int in       = get_input_fd();
	
	char* prompt = "\rEnter the recovery password: ";
	
	int idx      = 0;
	uint8_t c    = 0;
	int block_nb = 1;
	uint8_t digits[NB_DIGIT_BLOC + 1] = {0,};
	
	/* 8 = 7 hyphens separating the blocks + 1 '\0' at the end of the string */
	*rp = malloc(NB_RP_BLOCS * NB_DIGIT_BLOC + 8);
	memset(*rp, 0, NB_RP_BLOCS * NB_DIGIT_BLOC + 8);
	
	uint8_t* blah = *rp;
	
	printf("%s", prompt);
	fflush(NULL);
	
	fd_set rfds;
	int    nfds = in + 1;
	
	FD_ZERO(&rfds);
	FD_SET(in, &rfds);
	
	while(1)
	{
		/* Wait for inputs */
		int selret = select(nfds+1, &rfds, NULL, NULL, NULL);
		
		/* Look for errors */
		if(selret == -1)
		{
			fprintf(stderr, "Error %d in select: %s\n", errno, strerror(errno));
			break;
		}
		
		
		if(read(in, &c, 1) <= 0)
		{
			fprintf(stderr, "Something is available for reading but unable to "
					"read (%d): %s\n", errno, strerror(errno));
			break;
		}
		
		/* If this is an hyphen, just ignore it */
		if(c == '-')
			continue;
		
		/* Place the character at the right place or erase the last character */
		if(idx <= NB_DIGIT_BLOC)
		{
			/* If backspace was hit */
			if(c == '\b' || c == '\x7f')
			{
				idx--;
				
				if(idx < 0 && block_nb > 1)
				{
					blah -= (NB_DIGIT_BLOC + 1);
					snprintf((char*)digits, NB_DIGIT_BLOC, "%s", blah);
					*blah = '\0';
					idx   =  NB_DIGIT_BLOC - 1;
					block_nb--;
				}
				
				if(idx < 0)
					idx = 0;
				
				/* Yeah, I agree, this is kinda dirty */
				digits[idx] = ' ';
				printf("%s%s%s", prompt, *rp, digits);
				
				digits[idx] = '\0';
				idx--;
			}
			else if(c >= '0' && c <= '9')
				digits[idx] = (uint8_t)c;
			else
				continue;
		}
		
		printf("%s%s%s", prompt, *rp, digits);
		fflush(NULL);
		idx++;
		
		/* Now if we're at the end of a block, (in)validate it */
		if(idx >= NB_DIGIT_BLOC)
		{
			if(valid_block(digits, block_nb, NULL))
			{
				snprintf((char*)blah, NB_DIGIT_BLOC+1, "%s", digits);
				blah += NB_DIGIT_BLOC;
				
				if(block_nb >= NB_RP_BLOCS)
				{
					printf("\nValid password, continuing.\n");
					close_input_fd();
					return TRUE;
				}
				else
				{
					putchar('-');
					*blah = '-';
					blah++;
				}
				
				block_nb++;
			}
			else
			{
				fprintf(stderr, "\nInvalid block.\n");
				printf("%s%s", prompt, *rp);
			}
			
			fflush(NULL);
			
			idx = 0;
			memset(digits, 0, NB_DIGIT_BLOC);
		}
	}
	
	close_input_fd();
	return FALSE;
}


/**
 * Print the result key which can be used to decrypt the associated VMK
 * 
 * @param result_key The key after passed to intermediate_key
 */
void print_intermediate_key(uint8_t *result_key)
{
	if(result_key == NULL)
		return;
	
	int loop = 0;
	char s[32*3 + 1] = {0,};
	for(loop = 0; loop < 32; ++loop)
		snprintf(&s[loop*3], 4, "%02x ", result_key[loop]);
	
	xprintf(L_INFO, "Intermediate recovery key:\n\t%s\n", s);
}


