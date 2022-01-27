/*
 * Copyright (c) 2017, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Client application for writing data securely to the EEPROM via OP-TEE OS.
 */

#include <err.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* OP-TEE TEE client API (built by optee_client) */
#include <tee_client_api.h>

/* For the UUID (found in the TA's h-file(s)) */
#include <eepromEncrypt_sec_ta.h>

#define AES_TEST_BUFFER_SIZE	4096
#define AES_128_KEY_BYTE_SIZE	16
#define AES_BLOCK_SIZE		16

#define DECODE			0
#define ENCODE			1

/* TEE resources */
struct test_ctx {
	TEEC_Context ctx;
	TEEC_Session sess;
};


void print_usage(void)
{
        const char *writer_usage =
                "Usage:\n"
                "eepromEncrypt_sec -r 0x[addr(16-Bit)] [count Bytes]\n"
                "eepromEncrypt_sec -w 0x[addr(16-Bit)] [\"input_string\"]\n"
		"eepromEncrypt_sec --set_key [keyfile(128-Bit Key)]\n";

                fprintf(stderr, "%s\n", writer_usage);
                exit(1);
}

void prepare_tee_session(struct test_ctx *ctx)
{
	TEEC_UUID uuid = TA_EEENC_SEC_UUID;
	uint32_t origin;
	TEEC_Result res;

	/* Initialize a context connecting us to the TEE */
	res = TEEC_InitializeContext(NULL, &ctx->ctx);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InitializeContext failed with code 0x%x", res);

	/* Open a session with the TA */
	res = TEEC_OpenSession(&ctx->ctx, &ctx->sess, &uuid,
			       TEEC_LOGIN_PUBLIC, NULL, NULL, &origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_Opensession failed with code 0x%x origin 0x%x",
			res, origin);
}

/*
 * close the session and the context
 */
void terminate_tee_session(struct test_ctx *ctx)
{
	TEEC_CloseSession(&ctx->sess);
	TEEC_FinalizeContext(&ctx->ctx);
}

/*
 * Prepares the Ciphering operation by setting the AES encryption
 * mode, the key size and if there should be decoded or encoded.
 */
void prepare_aes(struct test_ctx *ctx, int encode)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;

	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE);

	op.params[0].value.a = TA_AES_ALGO_CTR;
	op.params[1].value.a = TA_AES_SIZE_128BIT;
	op.params[2].value.a = encode ? TA_AES_MODE_ENCODE :
					TA_AES_MODE_DECODE;

	res = TEEC_InvokeCommand(&ctx->sess, TA_AES_CMD_PREPARE,
				 &op, &origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InvokeCommand(PREPARE) failed 0x%x origin 0x%x",
			res, origin);
}

/* 
 * sets the invocation vector required for the AES counter
 * mode (CTR). 
 */
void set_iv(struct test_ctx *ctx, char *iv, size_t iv_sz)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;

	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					  TEEC_NONE, TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = iv;
	op.params[0].tmpref.size = iv_sz;

	res = TEEC_InvokeCommand(&ctx->sess, TA_AES_CMD_SET_IV,
				 &op, &origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InvokeCommand(SET_IV) failed 0x%x origin 0x%x",
			res, origin);
}

/*
 * Performs the actual ciphering (encode/decode) operation on memory read/written to the
 * EEPROM
 *
 * eepromaddress		the address to be written into on the eeprom
 * sz				size of the buffer to be written into / read from
 */
void cipher_buffer(struct test_ctx *ctx, char *in, char *out, size_t sz, uint32_t eepromAddress)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;

	memset(&op, 0, sizeof(op));
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_MEMREF_TEMP_OUTPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE);
	op.params[0].tmpref.buffer = in;
	op.params[0].tmpref.size = sz;
	op.params[1].tmpref.buffer = out;
	op.params[1].tmpref.size = sz;

	op.params[2].value.a = eepromAddress;

	res = TEEC_InvokeCommand(&ctx->sess, TA_AES_CMD_CIPHER,
				 &op, &origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InvokeCommand(CIPHER) failed 0x%x origin 0x%x",
			res, origin);
}

/* 
 * writes a 128-Bit AES key file to the OP-TEE secure Storage 
 *
 * data			pointer to a buffer that contains the key
 * data_len		size of the key buffer 
 */
TEEC_Result write_secure_object(struct test_ctx *ctx, char *data, size_t data_len)
{	
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;
	
	memset(&op, 0, sizeof(op));

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_NONE, TEEC_NONE,
					 TEEC_NONE);
	op.params[0].tmpref.buffer = data;
	op.params[0].tmpref.size = data_len;

	res = TEEC_InvokeCommand(&ctx->sess, TA_AES_CMD_WRITE_RAW,
				 &op, &origin);
	if (res != TEEC_SUCCESS)
		printf("Writing key failed: 0x%x / %u\n", res, origin);

	return res;	
}

int main(int argc, char *argv[])
{
	struct test_ctx ctx;
	char key[AES_128_KEY_BYTE_SIZE];
	char iv[AES_BLOCK_SIZE];
	char clear[AES_TEST_BUFFER_SIZE];
	char ciph[AES_TEST_BUFFER_SIZE];
	char temp[AES_TEST_BUFFER_SIZE];
	uint32_t file_size;
	
	uint32_t eepromAddress;

	FILE *fp;

	if (argc == 4  && argv[1][1] == 'w'){
		prepare_tee_session(&ctx);
        	eepromAddress = (uint32_t)strtol(&argv[2][2], NULL, 16);

		printf("Prepare encode operation: %d\n", ENCODE);
		prepare_aes(&ctx, ENCODE);

		printf("Reset ciphering operation in TA (provides the initial vector)\n");
		set_iv(&ctx, iv, AES_BLOCK_SIZE);

		printf("Encode buffer from TA\n");

		/* zero String buffer */
		memset(clear, 0, sizeof(clear));
		strcpy(clear, argv[3]);
				
		cipher_buffer(&ctx, clear, ciph, AES_TEST_BUFFER_SIZE, eepromAddress);

		terminate_tee_session(&ctx);
	} else if (argc == 4 && argv[1][1] == 'r') {

		prepare_tee_session(&ctx); 
                eepromAddress = (uint32_t)strtol(&argv[2][2], NULL, 16);

		printf("Prepare decode operation\n");
		prepare_aes(&ctx, DECODE);

		printf("Reset ciphering operation in TA (provides the initial vector)\n");
		memset(iv, 0, sizeof(iv)); /* Load some dummy value */
		set_iv(&ctx, iv, AES_BLOCK_SIZE);

		printf("Decode buffer from TA\n");
		cipher_buffer(&ctx, ciph, temp, AES_TEST_BUFFER_SIZE, eepromAddress);

		int read_count = atoi(argv[3]);
		
		/* print out count bytes of decrypted memory */
		printf("Decrypted %d Bytes from the EEPROM:\n", read_count);
		for(int i = 0; i < read_count; i++){
			printf("%c", temp[i]);
		}
		
		printf("\n");

		terminate_tee_session(&ctx);
	} else if (argc == 3 && argv[1][2] == 's') {
		
		prepare_tee_session(&ctx);
		
		/* load AES key from a specified file into key array */
		fp = fopen(argv[2], "r");

		fseek(fp, 0, SEEK_END); // seek to end of file
		file_size = ftell(fp); // get current file pointer
		fseek(fp, 0, SEEK_SET); // seek back to beginning of file

		/* check key size */
		if (file_size != 17) {
			errx(1, "Incorrect key size: %d, expected 16", file_size);
		}

		fread(key, AES_128_KEY_BYTE_SIZE, 1, fp);

		TEEC_Result res;
		res = write_secure_object(&ctx, key, sizeof(key));

		if (res != TEEC_SUCCESS)
			errx(1, "Failed to set key");

		terminate_tee_session(&ctx);
 
	} else {
		print_usage();
	}

	return 0;
}
