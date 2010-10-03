#include <ctype.h>
#include <getopt.h>
#include "common.h"
#include "openssl/aes.h"
#include "openssl/sha.h"
#include "openssl/evp.h"
#include "openssl/err.h"
#include "openssl/rand.h"

/* test encryption of "hello world" under policy of "ONE or TWO" */
#define MAX_ATTRIBUTES 100
Bool abe_decrypt(FENC_SCHEME_TYPE scheme, char *public_params, char *inputfile, char *keyfile);
void tokenize_inputfile(char* in, char** abe, char** aes);

/* 
 Description: abe-dec takes two inputs: an encrypted file and a private key and
 produces a file w/ the contents of the plaintext.
 NOTE: an exit code of 0 means ABE decryption was successful, 1 not successful, 
	   and -1 an ERROR with either the input file and/or command line arguments.
 */
int main (int argc, char *argv[]) {
	int fflag = FALSE, kflag = FALSE;
	char *file = "input.txt", *key = "private.key", *public_params = NULL;
	FENC_SCHEME_TYPE mode = FENC_SCHEME_NONE;
	int c;
	
	opterr = 0;

	while ((c = getopt (argc, argv, "m:f:k:h")) != -1) {
		
		switch (c)
		{
			case 'f': // file that holds encrypted data
				fflag = TRUE;
				file = optarg;
				printf("encrypted file = '%s'\n", file);
				break;
			case 'k': // input of private key 
				kflag = TRUE;
				key = optarg;
				printf("private-key file = '%s'\n", key);
				break;
			case 'm': 
				if (strcmp(optarg, SCHEME_LSW) == 0) {
					printf("Decrypting under Lewko-Sahai-Waters KP scheme...\n");
					mode = FENC_SCHEME_LSW;
					public_params = PUBLIC_FILE".kp";
				}
				else if(strcmp(optarg, SCHEME_WCP) == 0) {
					printf("Decrypting under Waters CP scheme...\n");
					mode = FENC_SCHEME_WATERSCP;
					public_params = PUBLIC_FILE".cp";
				}
				break;
			case 'h': // print usage 
				print_help();
				exit(0);
				break;
			case '?':
				if (optopt == 'f' || optopt == 'k')
					fprintf (stderr, "Option -%c requires an argument.\n", optopt);
				else if (isprint (optopt))
					fprintf (stderr, "Unknown option `-%c'.\n", optopt);
				else
					fprintf (stderr,
							 "Unknown option character `\\x%x'.\n", optopt);
				return -1;
			default:
				print_help();
				exit(-1);
		}
	}

	if(fflag == FALSE) {
		fprintf(stderr, "No file to decrypt!\n");
		goto error;
	}
	
	if(kflag == FALSE) {
		fprintf(stderr, "Decrypt without a key? c'mon!\n");
		goto error;	
	}
	
	if(mode == FENC_SCHEME_NONE) {
		fprintf(stderr, "Please specify a scheme type\n");
		goto error;	
	}
	return abe_decrypt(mode, public_params, file, key);
error:
	print_help();
	exit(-1);
}

void print_help(void)
{
	printf("Usage: ./abe-dec -m [ KP or CP ] -k [ private-key-file ] -f [ file-to-decrypt ] \n\n");
}
/* This function tokenizes the input file with the 
expected format: "ABE_TOKEN : base-64 : ABE_TOKEN_END : 
  			      AES_TOKEN : base-64 : AES_TOKEN_END"
 */
void tokenize_inputfile(char* in, char** abe, char** aes) 
{	
	ssize_t abe_len, aes_len;
	char delim[] = ":";
	char *token = strtok(in, delim);
	while (token != NULL) {
		if(strcmp(token, ABE_TOKEN) == 0) {
			token = strtok(NULL, delim);
			abe_len = strlen(token);
			if((*abe = (char *) malloc(abe_len+1)) != NULL) {
				strncpy(*abe, token, abe_len);
			}
		}
		else if(strcmp(token, AES_TOKEN) == 0) {
			token = strtok(NULL, delim);
			aes_len = strlen(token);
			if((*aes = (char *) malloc(aes_len+1)) != NULL) {
				strncpy(*aes, token, aes_len);
			}
		}
		token = strtok(NULL, delim);
	}
}

Bool abe_decrypt(FENC_SCHEME_TYPE scheme, char *public_params, char *inputfile, char *keyfile)
{
	FENC_ERROR result;
	fenc_context context;
	fenc_group_params group_params;
	fenc_global_params global_params;
	fenc_ciphertext ciphertext;
	fenc_plaintext aes_session_key;
	pairing_t pairing;
	fenc_key secret_key;
	FILE *fp;
	char c;
	int pub_len = 0;
	size_t serialized_len = 0;
	uint8 public_params_buf[SIZE];
	char output_str[200];
	int output_str_len = 200;
	int magic_failed;
	/* Clear data structures. */
	memset(&context, 0, sizeof(fenc_context));
	memset(&group_params, 0, sizeof(fenc_group_params));
	memset(&global_params, 0, sizeof(fenc_global_params));	
	memset(&public_params_buf, 0, SIZE);
	memset(&ciphertext, 0, sizeof(fenc_ciphertext));
	memset(&aes_session_key, 0, sizeof(fenc_plaintext));
	memset(public_params_buf, 0, SIZE);
	memset(output_str, 0, output_str_len);
	memset(&secret_key, 0, sizeof(fenc_key));
	// all this memory must be free'd 
	char *input_buf = NULL,*keyfile_buf = NULL;
	char *aes_blob64 = NULL, *abe_blob64 = NULL;
	ssize_t input_len, key_len;
	
	/* Load user's input file */
	fp = fopen(inputfile, "r");
	if(fp != NULL) {
		if((input_len = read_file(fp, &input_buf)) > 0) {
			// printf("Input file: %s\n", input_buf);
			tokenize_inputfile(input_buf, &abe_blob64, &aes_blob64);
#ifdef DEBUG 			
			printf("abe_blob64 = '%s'\n", abe_blob64);
			printf("aes_blob64 = '%s'\n", aes_blob64);
#endif
			free(input_buf);
		}			
	}
	else {
		fprintf(stderr, "Could not load input file: %s\n", inputfile);
		return FALSE;
	}
	fclose(fp);
	
	/* make sure the abe and aes ptrs are set */
	if(aes_blob64 == NULL || abe_blob64 == NULL) {
		fprintf(stderr, "Input file either not well-formed or not encrypted.\n");
		return FALSE;
	}
	
	/* Initialize the library. */
	result = libfenc_init();
	/* Create a Sahai-Waters context. */
	result = libfenc_create_context(&context, scheme);	
	/* Load group parameters from a file. */
	fp = fopen(PARAM, "r");
	if (fp != NULL) {
		libfenc_load_group_params_from_file(&group_params, fp);
		libfenc_get_pbc_pairing(&group_params, pairing);
	} else {
		fprintf(stderr, "File does not exist: global parmeterers");
		return FALSE;
	}
	fclose(fp);
	
	/* Set up the global parameters. */
	result = context.generate_global_params(&global_params, &group_params);
	report_error("Loading global parameters", result);
	
	result = libfenc_gen_params(&context, &global_params);
	// report_error("Generating scheme parameters and secret key", result);
	
	/* read file */
	fp = fopen(public_params, "r");
	if(fp != NULL) {
		while (TRUE) {
			c = fgetc(fp);
			if(c != EOF) {
				public_params_buf[pub_len] = c;
				pub_len++;
			}
			else {
				break;
			}
		}
	}
	else {
		fprintf(stderr, "File does not exist: %s\n", public_params);
		return FALSE;
	}
	fclose(fp);
	// printf("public params input = '%s'\n", public_params_buf);
	
	/* base-64 decode public parameters */
	uint8 *bin_public_buf = NewBase64Decode((const char *) public_params_buf, pub_len, &serialized_len);
	// printf("public params binary = '%s'\n", bin_public_buf);
	
	/* Import the parameters from binary buffer: */
	result = libfenc_import_public_params(&context, bin_public_buf, serialized_len);
	report_error("Importing public parameters", result);
	
	/* read input key file */ // (PRIVATE KEY)
	printf("keyfile => '%s'\n", keyfile);
	fp = fopen(keyfile, "r");
	if(fp != NULL) {
		if((key_len = read_file(fp, &keyfile_buf)) > 0) {
			// printf("\nYour private-key:\t'%s'\n", keyfile_buf);
			size_t keyLength;
			uint8 *bin_keyfile_buf = NewBase64Decode((const char *) keyfile_buf, key_len, &keyLength);

//#ifdef DEBUG			
			/* base-64 decode user's private key */
			printf("Base-64 decoded buffer:\t");
			print_buffer_as_hex(bin_keyfile_buf, keyLength);
//#endif			
			result = libfenc_import_secret_key(&context, &secret_key, bin_keyfile_buf, keyLength);
			report_error("Importing secret key", result);
			free(keyfile_buf);
		}			
	}
	else {
		fprintf(stderr, "Could not load input file: %s\n", keyfile);
		return FALSE;
	}
	fclose(fp);	

	size_t abeLength;
	uint8 *data = NewBase64Decode((const char *) abe_blob64, strlen(abe_blob64), &abeLength);
	ciphertext.data = data;
	ciphertext.data_len = abeLength;
	ciphertext.max_len = abeLength;	
	
	/* Descrypt the resulting ciphertext. */
	result = libfenc_decrypt(&context, &ciphertext, &secret_key, &aes_session_key);
	report_error("Decrypting the ciphertext", result);
	
	printf("\tDecrypted session key is: ");
	print_buffer_as_hex(aes_session_key.data, aes_session_key.data_len);

	/* decode the aesblob64 */
	size_t aesLength;
	char *aesblob = NewBase64Decode((const char *) aes_blob64, strlen(aes_blob64), &aesLength);
	
	/* use the PSK to encrypt using openssl functions here */
	AES_KEY sk;
	char iv[SESSION_KEY_LEN*4];
	char aes_result[aesLength+1];
	AES_set_decrypt_key((uint8 *) aes_session_key.data, 8*SESSION_KEY_LEN, &sk);
	memset(iv, 0, SESSION_KEY_LEN*4);
	memset(aes_result, 0, aesLength+1);
	AES_cbc_encrypt((uint8 *) aesblob, (uint8 *) aes_result, aesLength, &sk, (unsigned char *) iv, AES_DECRYPT);
	/* base-64 both ciphertext and write to the stdout -- in XML? */
	
	char magic[strlen(MAGIC)+1];
	memset(magic, 0, strlen(MAGIC)+1);
	strncpy(magic, aes_result, strlen(MAGIC));
	
	if(strcmp(magic, MAGIC) == 0) {
		printf("Recovered magic: '%s'\n", magic);		
		printf("Plaintext: %s\n", (char *) (aes_result + strlen(MAGIC)));
		magic_failed = FALSE;
	}
	else {
		fprintf(stderr, "ERROR: ABE Decryption unsuccessful!!!\n");
		magic_failed = TRUE;
	}
	
	/* Destroy the context. */
	result = libfenc_destroy_context(&context);
	report_error("Destroying the encryption context", result);	
	
	/* Shutdown the library. */
	result = libfenc_shutdown();
	report_error("Shutting down library", result);
	return magic_failed;
}

