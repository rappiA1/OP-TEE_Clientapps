/*
 * Author: Raphael Andree
 *
 * CA for testing the functions of the EEPROMwriter PTA
 */

#include <err.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <tee_client_api.h>

#define EPW_UUID \
        {0x2b6ea7b2, 0xaf6a, 0x4387, \
                {0xaa, 0xa7, 0x4c, 0xef, 0xcc, 0x4a, 0xfc, 0xbd}}

#define PTA_CMD_READ    0
#define PTA_CMD_WRITE   1
#define PTA_CMD_INIT    2


struct test_ctx {
	TEEC_Context ctx;
	TEEC_Session sess;
};

/*
 * Write one byte of data to the EEPROM
 *
 * ctx		contains the session data
 * data		buffer to be written into the EEPROM
 * data_length	Length of the data bufffer.
 */
TEEC_Result writeByte(struct test_ctx *ctx, 
		char *data, uint32_t data_length, uint32_t eepromAddress)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;
	
	/* Initialize Operation struct with zeroes. */
	memset(&op, 0, sizeof(op));

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE,
					 TEEC_NONE);
	op.params[0].tmpref.buffer = data;
	op.params[0].tmpref.size = data_length;

	/* slave address */
	op.params[1].value.a = 80;

	/* address on the eeprom. */
	op.params[1].value.b = eepromAddress;


	res = TEEC_InvokeCommand(&ctx->sess,
				 PTA_CMD_WRITE,
				 &op, &origin);

	if (res != TEEC_SUCCESS)
		printf("Writing to the EEPROM failed: 0x%x, / %u\n", res, origin);
	return res;
}

/*
 * read data from the EEPROM
 *
 * data_length		Number of bytes to read from the EEPROM
 * eepromAddress	Address on the EEPROM to read from
 */
TEEC_Result readByte(struct test_ctx *ctx,
		char *data, uint32_t data_length, uint32_t eepromAddress)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;

	memset(&op, 0, sizeof(op));

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_OUTPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE,
					 TEEC_NONE);

	op.params[0].tmpref.buffer = data;
	op.params[0].tmpref.size = data_length;

	//Hardwired EEPROM i2c device address
	op.params[1].value.a = 80;

	/* address on the EEPROM */
	op.params[1].value.b = eepromAddress;

	res = TEEC_InvokeCommand(&ctx->sess,
				 PTA_CMD_READ,
				 &op, &origin);

	if (res != TEEC_SUCCESS)
		printf("Reading from the EEPROM failed: 0x%x, / %u\n", res, origin);
	return res;
}

/* 
 * Invokes the initialization of the i2c controller and the EEPROM
 */
void startSession(struct test_ctx *ctx)
{
	TEEC_UUID uuid = EPW_UUID;
	uint32_t origin;
	TEEC_Result res;

	/* Initialize a context to connect to the TEE */
	res = TEEC_InitializeContext(NULL, &ctx->ctx);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_InitializeContext failed with code 0x%x", res);
	
	/* Open a session with the PTA */
	res = TEEC_OpenSession(&ctx->ctx, &ctx->sess, &uuid,
			TEEC_LOGIN_PUBLIC, NULL, NULL, &origin);
	if (res != TEEC_SUCCESS)
		errx(1, "TEEC_OpenSession failed with code 0x%x origin 0x%x",
			res, origin);
}

/*
 * Initialize the I2C controller inside the eepromWriterPTA
 */
TEEC_Result initController(struct test_ctx *ctx)
{
	uint32_t origin;
	TEEC_Result res;

	res = TEEC_InvokeCommand(&ctx->sess,
			PTA_CMD_INIT, NULL, &origin);
	return res;
}

void print_usage(void)
{
	const char *writer_usage = 
		"Usage:\n\n"
		"EEPROMTester -r 0x[addr(16-Bit)] [count Bytes]\n"
		"EEPROMTester -w 0x[addr(16-Bit)] [string_without_whitespaces]\n";

		fprintf(stderr, "%s\n", writer_usage);
		exit(1);
}

/*
 * Main function for testing the read and write functionality of the EEPROM testing application.
 */
int main(int argc, char *argv[])
{
	struct test_ctx ctx;

	/* buffer that contains the parameters and the address */
	char writeBuffer[4096];
	unsigned int writeBufferLength;

	uint32_t eepromAddress;

	unsigned int bytes_to_read;
	char readBuffer[4096];
	TEEC_Result res;

	//TODO check if command line paramers have correct format.

	/* check argc size */
	if (argc >= 3){

		/* 
		 * read the EEPROM address from the 3rd command line parameter and write it into the
		 * integer which contains the address on the EEPROM, we ignore the first two characters because
		 * they only represent "0x" 
		 */
		eepromAddress = (uint32_t)strtol(&argv[2][2], NULL, 16);

		startSession(&ctx);

		/* if read flag is set */
		if (argv[1][1] == 'r'){
			
			bytes_to_read = atoi(argv[3]);

			/* initialize the i2c controller */
			res = initController(&ctx);

			/*read byte from the EEPROM */
			res = readByte(&ctx,readBuffer, bytes_to_read, eepromAddress);

			printf("%d bytes read Starting from %x%x:\n", bytes_to_read,
					writeBuffer[0], writeBuffer[1]);
			for (int i = 0; i < bytes_to_read; i++){
				printf("%x ", readBuffer[i]);
			}
			printf("\n");

		} else if (argv[1][1] == 'w'){
			
			/* data buffer length*/
			writeBufferLength = strlen(argv[3]);

			if (writeBufferLength > sizeof(writeBuffer)) {
				fprintf(stderr, "String size exceeds buffer size");
				exit(1);
			}

			/* write data after address into writeBuffer */
			strcpy(writeBuffer, argv[3]);

			/* initialize the i2c controller */
			res = initController(&ctx);

			/* write to the EEPROM */
			res = writeByte(&ctx, writeBuffer, writeBufferLength, eepromAddress);

			printf("%d Bytes written to EEPROM\n", writeBufferLength);
		} else {
			print_usage();
		}
		TEEC_CloseSession(&(ctx.sess));

		TEEC_FinalizeContext(&(ctx.ctx));

	} else {
		print_usage();
	}

	return res;
}
