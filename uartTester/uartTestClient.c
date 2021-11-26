/*
 * Author: Raphael Andree
 *
 * Small client application for testing the uartAccess PTA
 */

#include <err.h>
#include <stdio.h>
#include <tee_client_api.h>

#define UACCESS_UUID \
        {0x45cdb822, 0xa2ea, 0x483f, \
                {0x9e, 0x63, 0xd5, 0x9e, 0x11, 0x0e, 0xa5, 0x3f} }

#define PTA_CMD_PRINTTEXT	0

int main(void)
{
	TEEC_Result res;
	TEEC_Context ctx;
	TEEC_Session sess;
//	TEEC_Operation op;
	TEEC_UUID  uuid= UACCESS_UUID;
	uint32_t err_origin;

	/* Initialize a context with the TEE */
	res = TEEC_InitializeContext(NULL, &ctx);
	if (res)
		errx(1, "TEEC_InitializeContext failed with code 0x%x", res);

	/* Open session with the PTA */
	res = TEEC_OpenSession(&ctx, &sess, &uuid,
			TEEC_LOGIN_PUBLIC, NULL, NULL, &err_origin);
	if (res)
		errx(1, "TEEC_OpenSession failed with code 0x%x origin 0x%x",
				res, err_origin);

	printf("Invoking uartAccess PTA");
	res = TEEC_InvokeCommand(&sess, PTA_CMD_PRINTTEXT, NULL, &err_origin);

	/* Close the session and destroy the context */
	TEEC_CloseSession(&sess);

	TEEC_FinalizeContext(&ctx);

	return 0;

}
