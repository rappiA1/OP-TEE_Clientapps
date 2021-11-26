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
 * data		String to be written into the EEPROM, including the destination address.
 * data_length	Length of the data bufffer.
 */
TEEC_Result writeByte(struct test_ctx *ctx, 
		char *data, uint32_t data_length)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;
	
	/* Initialize Operation struct with zeroes. */
	memset(&op, 0, sizeof(op));

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE, TEEC_NONE);
	op.params[0].tmpref.buffer = data;
	op.params[0].tmpref.size = data_length;

//	char *bufferPointer = op.params[0].tmpref.buffer;

//	printf("value of writeBuffer: %x%x%x\n", *bufferPointer, *(bufferPointer + 1 ), *(bufferPointer + 2));

	/* slave address */
	op.params[1].value.a = 80;


	res = TEEC_InvokeCommand(&ctx->sess,
				 PTA_CMD_WRITE,
				 &op, &origin);

	if (res != TEEC_SUCCESS)
		printf("Writing to the EEPROM failed: 0x%x, / %u\n", res, origin);
	return res;
}

/*
 * read one byte of data from the EEPROM
 */
TEEC_Result readByte(struct test_ctx *ctx,
		char *address, uint32_t address_length,
		char *data, uint32_t data_length)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;

	memset(&op, 0, sizeof(op));

	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_MEMREF_TEMP_OUTPUT,
					 TEEC_VALUE_INPUT,
					 TEEC_NONE);
	printf("readbyte sets 0x%x%x to buffer\n", address[0], address[1]);
	op.params[0].tmpref.buffer = address;
	op.params[0].tmpref.size = address_length;

	op.params[1].tmpref.buffer = data;
	op.params[1].tmpref.size = data_length;

	//Hardwired EEPROM i2c device address
	op.params[2].value.a = 80;

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


void readAddressIntoBuffer(char *buffer, char *addressString)
{
	for (int n = 0; n < 4; n++){
			sscanf(addressString, "%2hhx", &buffer[n]);
			addressString += 2;
		}

}

/*
 * Main function for testing the read and write functionality of the EEPROM testing application.
 */
int main(int argc, char *argv[])
{
	struct test_ctx ctx;

	/* buffer that contains the parameters and the address */
	char writeBuffer[60];
	unsigned int writeBufferLength;

	unsigned int bytes_to_read;
	/* for now only one byte, later array, possibly greater size than data read.*/
	char readBuffer;
	TEEC_Result res;

	/* TODO check argc size */
	if (argc >= 3){

		/* 
		 * read the EEPROM address from the 3rd command line parameter and write it into the
		 * first two bytes of the writeBuffer, we ignore the first two characters because
		 * they only represent "0x" 
		 */
		readAddressIntoBuffer(writeBuffer, &argv[2][2]);

		/* 
		 * Minimum 2 address bytes get written to the EEPROM, i.e. at reading
		 */
		writeBufferLength = 2;

		startSession(&ctx);

		//TODO implement sequential read
		/* if read flag is set */
		if (argv[1][1] == 'r'){
			
			/* only one byte read, sequential read not implemented yet. */
			bytes_to_read = 1;

			/* initialize the i2c controller */
			res = initController(&ctx);

			/*read byte from the EEPROM */
			res = readByte(&ctx, writeBuffer, writeBufferLength, &readBuffer, bytes_to_read);

			printf("Read 0x%x from EEPROM at address 0x%x%x\n", readBuffer,
					writeBuffer[0], writeBuffer[1]);
		} else if (argv[1][1] == 'w'){
			
			//TODO size check on input string if it exceeds writebuffer size
			/* write data after address into the writeBuffer */
			strcpy(&writeBuffer[2], argv[3]);

			/* data length additional to the two address bytes */
			writeBufferLength += strlen(argv[3]);
			printf("WritebufferLength: %d\n", writeBufferLength);

			/* initialize the i2c controller */
			res = initController(&ctx);

			/* write to the EEPROM */
			res = writeByte(&ctx, writeBuffer, writeBufferLength);
		} else {
			print_usage();
		}

	} else {
		print_usage();
	}

	return res;
}
