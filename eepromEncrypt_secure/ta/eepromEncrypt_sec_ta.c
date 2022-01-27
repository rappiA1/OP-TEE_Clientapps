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
 *
 * TA for encrypting data and writing them to the EEPROM, works only with 128
 * Bit AES keys.
 */
#include <inttypes.h>

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>

#include <eepromEncrypt_sec_ta.h>

#define AES128_KEY_BIT_SIZE		128
#define AES128_KEY_BYTE_SIZE		(AES128_KEY_BIT_SIZE / 8)
#define AES256_KEY_BIT_SIZE		256
#define AES256_KEY_BYTE_SIZE		(AES256_KEY_BIT_SIZE / 8)

/* Test buffer size*/
#define AES_TEST_BUFFER_SIZE		4096

#define PTA_CMD_READ 	0
#define PTA_CMD_WRITE	1
#define PTA_CMD_INIT	2

/* UUID of EEPROMWriter PTA */
#define EPW_UUID \
        {0x2b6ea7b2, 0xaf6a, 0x4387, \
                {0xaa, 0xa7, 0x4c, 0xef, 0xcc, 0x4a, 0xfc, 0xbd}}

/*
 * Ciphering context: each opened session relates to a cipehring operation.
 * - configure the AES flavour from a command.
 * - load key from a command (here the key is provided by the REE)
 * - reset init vector (here IV is provided by the REE)
 * - cipher a buffer frame (here input and output buffers are non-secure)
 */
struct aes_cipher {
	uint32_t algo;			/* AES flavour */
	uint32_t mode;			/* Encode or decode */
	uint32_t key_size;		/* AES key size in byte */
	TEE_OperationHandle op_handle;	/* AES ciphering operation */
	TEE_ObjectHandle key_handle;	/* transient object to load the key */
};


/* 
 * initializes a Session with the EEPROMWriter and 
 * calls the EEPROMWriter i2c init function to initialize the
 * BSC. 
 */
static TEE_Result initSessionWithEPW (TEE_TASessionHandle *epwSession)
{
	TEE_UUID uuid = EPW_UUID;
	uint32_t origin;
	TEE_Result res;
	
	res = TEE_OpenTASession(&uuid, TEE_TIMEOUT_INFINITE,
		0, NULL, epwSession, &origin);
	
	res = TEE_InvokeTACommand(*epwSession, TEE_TIMEOUT_INFINITE,
		       	PTA_CMD_INIT, 0, NULL, &origin);
	return res;
}

/*
 * Few routines to convert IDs from TA API into IDs from OP-TEE.
 */
static TEE_Result ta2tee_algo_id(uint32_t param, uint32_t *algo)
{
	switch (param) {
	case TA_AES_ALGO_ECB:
		*algo = TEE_ALG_AES_ECB_NOPAD;
		return TEE_SUCCESS;
	case TA_AES_ALGO_CBC:
		*algo = TEE_ALG_AES_CBC_NOPAD;
		return TEE_SUCCESS;
	case TA_AES_ALGO_CTR:
		*algo = TEE_ALG_AES_CTR;
		return TEE_SUCCESS;
	default:
		EMSG("Invalid algo %u", param);
		return TEE_ERROR_BAD_PARAMETERS;
	}
}

/*
 * Convert the TA macro types to the macro types defined
 * by the GlobalPlatform specification.
 */
static TEE_Result ta2tee_key_size(uint32_t param, uint32_t *key_size)
{
	switch (param) {
	case AES128_KEY_BYTE_SIZE:
	case AES256_KEY_BYTE_SIZE:
		*key_size = param;
		return TEE_SUCCESS;
	default:
		EMSG("Invalid key size %u", param);
		return TEE_ERROR_BAD_PARAMETERS;
	}
}

/*
 * Convert the TA macro types to the macro types defined
 * by the GobalPlatform Specification.
 */
static TEE_Result ta2tee_mode_id(uint32_t param, uint32_t *mode)
{
	switch (param) {
	case TA_AES_MODE_ENCODE:
		*mode = TEE_MODE_ENCRYPT;
		return TEE_SUCCESS;
	case TA_AES_MODE_DECODE:
		*mode = TEE_MODE_DECRYPT;
		return TEE_SUCCESS;
	default:
		EMSG("Invalid mode %u", param);
		return TEE_ERROR_BAD_PARAMETERS;
	}
}

/* 
 * Write the key from the normal world to the secure Storage.
 */
static TEE_Result create_raw_object(uint32_t param_types, TEE_Param params[4])
{
	const uint32_t exp_param_types = 
		TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				TEE_PARAM_TYPE_NONE,
				TEE_PARAM_TYPE_NONE,
				TEE_PARAM_TYPE_NONE);
	TEE_ObjectHandle object;
	TEE_Result res;
	char obj_id[] = "TA_AES_Key";
	size_t obj_id_sz = sizeof(obj_id);
	char *data;
	size_t data_sz;
	uint32_t obj_data_flag;

	/* Safely get the invocation parameters */
	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	data_sz = params[0].memref.size;
	data = TEE_Malloc(data_sz, 0);
	if (!data)
		return TEE_ERROR_OUT_OF_MEMORY;
	TEE_MemMove(data, params[0].memref.buffer, data_sz);

	/* Create object in secure storage and fill with data */
	obj_data_flag = TEE_DATA_FLAG_ACCESS_READ | /* we can later read the object. */
			TEE_DATA_FLAG_ACCESS_WRITE | /* we can later write into the object */
			TEE_DATA_FLAG_ACCESS_WRITE_META | /* we can later destroy or rename object */
			TEE_DATA_FLAG_OVERWRITE;	/* we can destroy an object with same id */

	res = TEE_CreatePersistentObject(TEE_STORAGE_PRIVATE,
					 obj_id, obj_id_sz,
					 obj_data_flag,
					 TEE_HANDLE_NULL,
					 NULL, 0,
					 &object);
	if (res != TEE_SUCCESS) {
		EMSG("TEE_CreatePersistentObject failed with 0x%08x", res);
		TEE_Free(obj_id);
		TEE_Free(data);
		return res;
	}

	res = TEE_WriteObjectData(object, data, data_sz);
	if (res != TEE_SUCCESS) {
		EMSG("TEE_WriteObjectData failed with 0x%08x", res);
		TEE_CloseAndDeletePersistentObject(object);
	} else {
		TEE_CloseObject(object);
	}

	TEE_Free(data);
	return res;

}

/* 
 * reads the AES key content from the secure storage to a buffer.
 *
 * buffer		buffer that will store the key contents.
 * buffer_size		size of the key buffer.
 */
static TEE_Result load_secure_storage_key(char *buffer, uint32_t *buffer_size)
{
	TEE_ObjectHandle object;
	TEE_ObjectInfo object_info;
	TEE_Result res;
	uint32_t read_bytes;
	char obj_id[] = "TA_AES_Key";
	size_t obj_id_sz = sizeof(obj_id);
	char *data;
	size_t data_sz;

	data_sz = *buffer_size;
	data = TEE_Malloc(data_sz, 0);
	if (!data)
		return TEE_ERROR_OUT_OF_MEMORY;


	/*
	 * Check the object exist and can be dumped into output buffer
	 * then dump it.
	 */
	res = TEE_OpenPersistentObject(TEE_STORAGE_PRIVATE,
					obj_id, obj_id_sz,
					TEE_DATA_FLAG_ACCESS_READ |
					TEE_DATA_FLAG_SHARE_READ,
					&object);
	if (res != TEE_SUCCESS) {
		EMSG("Failed to open persistent object, res=0x%08x", res);
		TEE_Free(obj_id);
		TEE_Free(data);
		return res;
	}

	res = TEE_GetObjectInfo1(object, &object_info);
	if (res != TEE_SUCCESS) {
		EMSG("Failed to create persistent object, res=0x%08x", res);
		goto exit;
	}

	if (object_info.dataSize > data_sz) {
		/*
		 * Provided buffer is too short.
		 * Return the expected size together with status "short buffer"
		 */
		//params[1].memref.size = object_info.dataSize;
		res = TEE_ERROR_SHORT_BUFFER;
		goto exit;
	}

	res = TEE_ReadObjectData(object, data, object_info.dataSize,
				 &read_bytes);
	if (res == TEE_SUCCESS)
		TEE_MemMove(buffer, data, read_bytes);
	if (res != TEE_SUCCESS || read_bytes != object_info.dataSize) {
		EMSG("TEE_ReadObjectData failed 0x%08x, read %" PRIu32 " over %u",
				res, read_bytes, object_info.dataSize);
		goto exit;
	}

	/* Return the number of bytes effectively filled */
	*buffer_size = read_bytes;
exit:
	TEE_CloseObject(object);
	TEE_Free(data);
	return res;
}

/*
 * set the key in the aes_cipher struct.
 *
 * session		session unique pointer which points to a pointer
 * 			that points to the memory with the aes_cipher struct.
 */
static TEE_Result set_aes_key(void *session)
{
	struct aes_cipher *sess;
	TEE_Attribute attr;
	TEE_Result res;
	uint32_t key_sz;
	char key[AES128_KEY_BYTE_SIZE];

	/* Get ciphering context from session ID */
	DMSG("Session %p: load key material", session);
	sess = (struct aes_cipher *)session;

	key_sz = AES128_KEY_BYTE_SIZE;

	if (key_sz != sess->key_size) {
		EMSG("Wrong key size %" PRIu32 ", expect %" PRIu32 " bytes",
		     key_sz, sess->key_size);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	/*
	 * Load the key material into the configured operation
	 * - create a secret key attribute with the key material
	 *   TEE_InitRefAttribute()
	 * - reset transient object and load attribute data
	 *   TEE_ResetTransientObject()
	 *   TEE_PopulateTransientObject()
	 * - load the key (transient object) into the ciphering operation
	 *   TEE_SetOperationKey()
	 *
	 * TEE_SetOperationKey() requires operation to be in "initial state".
	 * We can use TEE_ResetOperation() to reset the operation but this
	 * API cannot be used on operation with key(s) not yet set. Hence,
	 * when allocating the operation handle, we load a dummy key.
	 * Thus, set_key sequence always reset then set key on operation.
	 */

	/* Load the key from the secure Storage into the key buffer */
	load_secure_storage_key(key, &key_sz);

	TEE_InitRefAttribute(&attr, TEE_ATTR_SECRET_VALUE, key, key_sz);

	TEE_ResetTransientObject(sess->key_handle);
	res = TEE_PopulateTransientObject(sess->key_handle, &attr, 1);
	if (res != TEE_SUCCESS) {
		EMSG("TEE_PopulateTransientObject failed, %x", res);
		return res;
	}
	
	TEE_ResetOperation(sess->op_handle);
	res = TEE_SetOperationKey(sess->op_handle, sess->key_handle);
	if (res != TEE_SUCCESS) {
		EMSG("TEE_SetOperationKey failed %x", res);
		return res;
	}

	return res;
}

/*
 * Process command TA_AES_CMD_SET_IV. Set initialization vector.
 */
static TEE_Result reset_aes_iv(void *session, uint32_t param_types,
				TEE_Param params[4])
{
	const uint32_t exp_param_types =
		TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				TEE_PARAM_TYPE_NONE,
				TEE_PARAM_TYPE_NONE,
				TEE_PARAM_TYPE_NONE);
	struct aes_cipher *sess;
	size_t iv_sz;
	char *iv;

	/* Get ciphering context from session ID */
	DMSG("Session %p: reset initial vector", session);
	sess = (struct aes_cipher *)session;

	/* Safely get the invocation parameters */
	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	iv = params[0].memref.buffer;
	iv_sz = params[0].memref.size;

	/*
	 * Init cipher operation with the initialization vector.
	 */
	TEE_CipherInit(sess->op_handle, iv, iv_sz);

	return TEE_SUCCESS;
}

/*
 * Writes cipher buffer to the EEPROM by calling EEPROMWriter PTA.
 *
 * pta_sess		Handle to the session with the eepromWriter PTA (EEPROM Driver)
 * data			Encrypted memory buffer
 * data_length		Size of the encrypted Memory buffer
 * eepromAddress	Destination address on the EEPROM
 */
static TEE_Result writeEEPROM(TEE_TASessionHandle pta_sess,
		char *data, uint32_t data_length, uint32_t eepromAddress)
{
	uint32_t origin;
	TEE_Result res;
	TEE_Param params[4];
	uint32_t paramTypes = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
					      TEE_PARAM_TYPE_VALUE_INPUT,
					      TEE_PARAM_TYPE_NONE,
					      TEE_PARAM_TYPE_NONE);
	params[0].memref.buffer = data;
	params[0].memref.size = data_length;

	/* i2c Slave Address (EEPROM) */
	params[1].value.a = 80;

	params[1].value.b = eepromAddress;

	/*
	 * Invoke Write command in EEPROMWriter PTA.
	 *
	 * TEE_InvokeTACommand takes a little bit different parameters
	 * ase TEEC_invokeCommand, see GlobalPlatform documentation.
	 */
	res = TEE_InvokeTACommand(pta_sess, TEE_TIMEOUT_INFINITE,
			    PTA_CMD_WRITE, paramTypes, params, &origin);	
	if (res != TEE_SUCCESS)
		EMSG("Writing to the EEPROM failed: 0x%x, / %u", res, origin);

	return res;
}

/*
 * Read ciphered Data from the EEPROM.
 *
 * pta_sess		Handle to the session with the eepromWriter PTA (EEPROM Driver)
 * data			Encrypted memory buffer
 * data_length		Size of the encrypted Memory buffer
 * eepromAddress	Destination address on the EEPROM
 */
static TEE_Result readEEPROM(TEE_TASessionHandle pta_sess, char *data, uint32_t data_length,
		uint32_t eepromAddress)
{	
	uint32_t origin;
	TEE_Result res;
	TEE_Param params[4];
	uint32_t paramTypes = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_OUTPUT,
					      TEE_PARAM_TYPE_VALUE_INPUT,
					      TEE_PARAM_TYPE_NONE,
					      TEE_PARAM_TYPE_NONE);
	params[0].memref.buffer = data;
	params[0].memref.size = data_length;

	/* i2c slave address (EEPROM) */
	params[1].value.a = 80;

	/* address on the EEPROM */
	params[1].value.b = eepromAddress;

	res = TEE_InvokeTACommand(pta_sess, TEE_TIMEOUT_INFINITE,
			PTA_CMD_READ, paramTypes, params, &origin);
	if (res != TEE_SUCCESS)
		EMSG("Reading from the EEPROM failed> 0x%x, / %u", res, origin);
	return res;
}


/*
 * Process command TA_AES_CMD_CIPHER. Encrypt and decrypt buffer.
 * 
 * session		pointer to the memory of the aes_cipher struct.
 */
static TEE_Result cipher_buffer(void *session, uint32_t param_types,
				TEE_Param params[4])
{
	const uint32_t exp_param_types =
		TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INPUT,
				TEE_PARAM_TYPE_MEMREF_OUTPUT,
				TEE_PARAM_TYPE_VALUE_INPUT,
				TEE_PARAM_TYPE_NONE);
	struct aes_cipher *sess;

	/* SessionHandle for the session with the EEPROMWriter PTA */
	TEE_TASessionHandle pta_sess;
	TEE_Result res;

	uint32_t eepromAddress;

	char tempReadBuffer[AES_TEST_BUFFER_SIZE];
	char tempWriteBuffer[AES_TEST_BUFFER_SIZE];
	
	/* Address on EEPROM as parameter from Normal World CA */
	eepromAddress = params[2].value.a;

	//DMSG("EEPROMADDRESS: %x", eepromAddress);
	
	/* number of bytes to write */
	unsigned int writeBufferLength;

	/* Get ciphering context from session ID */
	DMSG("Session %p: cipher buffer", session);
	sess = (struct aes_cipher *)session;

	/* Safely get the invocation parameters */
	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	if (params[1].memref.size < sizeof(tempReadBuffer)) {
		EMSG("Bad sizes: in %d, out %ld", params[0].memref.size,
						 sizeof(tempReadBuffer));
		return TEE_ERROR_BAD_PARAMETERS;
	}

	/* wrong state if operation handle is not initialized yet */
	if (sess->op_handle == TEE_HANDLE_NULL)
		return TEE_ERROR_BAD_STATE;

	/* 
	 * Create Session with EEPROMWriter PTA (EEPROM Driver) and call
	 * init function
	 */
	DMSG("Initializing Session with EEPROMWriter");
	res = initSessionWithEPW(&pta_sess);

	DMSG("MODE: %d", sess->mode);

	if (sess->mode == TEE_MODE_ENCRYPT) {
		
		/* write ciphered data after address into the writeBuffer */
		res = TEE_CipherUpdate(sess->op_handle,
				params[0].memref.buffer, params[0].memref.size,
				tempWriteBuffer, &params[0].memref.size);

		DMSG("write buffer to EEPROM address: 0x%x", eepromAddress);

		/* Increase the writeBufferLength by the size of the buffer provided by the CA */
		writeBufferLength = params[0].memref.size;

		/* Write ciphered Data to the EEPROM */
		res = writeEEPROM(pta_sess, tempWriteBuffer, writeBufferLength, eepromAddress);

	}
	else if (sess->mode == TEE_MODE_DECRYPT) {
		DMSG("read buffer from EEPROM");
		/* read tempBuffer from the EEPROM */
		res = readEEPROM(pta_sess, tempReadBuffer, params[1].memref.size, eepromAddress); 	

       		res = TEE_CipherUpdate(sess->op_handle,
				tempReadBuffer, sizeof(tempReadBuffer),
				params[1].memref.buffer, &params[1].memref.size);
	}
	else {
		EMSG("Incorrect cipher mode");
		return TEE_ERROR_BAD_PARAMETERS;
	}

	/* close the session to the EEPROMWriter PTA */
	DMSG("Closing EPW TA_session");
	TEE_CloseTASession(pta_sess);

	return res;
}

TEE_Result TA_CreateEntryPoint(void)
{
	/* Nothing to do */
	return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
	/* Nothing to do */
}

/*
 * called when a session to the TA is created. Allocates
 * memory for the aes_cipher struct, that will contain encryption info.
 */
TEE_Result TA_OpenSessionEntryPoint(uint32_t __unused param_types,
					TEE_Param __unused params[4],
					void __unused **session)
{
	struct aes_cipher *sess;

	/*
	 * Allocate and init ciphering materials for the session.
	 * The address of the structure is used as session ID for
	 * the client. (set as session_id in the normal world session struct,
	 * see tee_client_api.h)
	 */
	sess = TEE_Malloc(sizeof(*sess), 0);
	if (!sess)
		return TEE_ERROR_OUT_OF_MEMORY;

	sess->key_handle = TEE_HANDLE_NULL;
	sess->op_handle = TEE_HANDLE_NULL;

	*session = (void *)sess;
	DMSG("Session %p: newly allocated", *session);

	return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *session)
{
	struct aes_cipher *sess;

	/* Get ciphering context from session ID */
	DMSG("Session %p: release session", session);
	sess = (struct aes_cipher *)session;

	/* Release the session resources */
	if (sess->key_handle != TEE_HANDLE_NULL)
		TEE_FreeTransientObject(sess->key_handle);

	if (sess->op_handle != TEE_HANDLE_NULL)
		TEE_FreeOperation(sess->op_handle);
	
	TEE_Free(sess);
}

/*
 * prepares ciphering operation
 */
static TEE_Result alloc_resources(void *session, uint32_t param_types,
				  TEE_Param params[4])
{
	const uint32_t exp_param_types =
		TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
				TEE_PARAM_TYPE_VALUE_INPUT,
				TEE_PARAM_TYPE_VALUE_INPUT,
				TEE_PARAM_TYPE_NONE);
	struct aes_cipher *sess;
	TEE_Attribute attr;
	TEE_Result res;
	char *key;

	/* Get ciphering context from session ID */
	DMSG("Session %p: get ciphering resources", session);
	sess = (struct aes_cipher *)session;

	/* Safely get the invocation parameters */
	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	res = ta2tee_algo_id(params[0].value.a, &sess->algo);
	if (res != TEE_SUCCESS)
		return res;

	res = ta2tee_key_size(params[1].value.a, &sess->key_size);
	if (res != TEE_SUCCESS)
		return res;

	res = ta2tee_mode_id(params[2].value.a, &sess->mode);
	if (res != TEE_SUCCESS)
		return res;

	/*
	 * Ready to allocate the resources which are:
	 * - an operation handle, for an AES ciphering of given configuration
	 * - a transient object that will be use to load the key materials
	 *   into the AES ciphering operation.
	 */

	/* Free potential previous operation */
	if (sess->op_handle != TEE_HANDLE_NULL)
		TEE_FreeOperation(sess->op_handle);

	/* Allocate operation: AES/CTR, mode and size from params */
	res = TEE_AllocateOperation(&sess->op_handle,
				    sess->algo,
				    sess->mode,
				    sess->key_size * 8);
	if (res != TEE_SUCCESS) {
		EMSG("Failed to allocate operation");
		sess->op_handle = TEE_HANDLE_NULL;
		goto err;
	}

	/* Free potential previous transient object */
	if (sess->key_handle != TEE_HANDLE_NULL)
		TEE_FreeTransientObject(sess->key_handle);

	/* Allocate transient object according to target key size */
	res = TEE_AllocateTransientObject(TEE_TYPE_AES,
					  sess->key_size * 8,
					  &sess->key_handle);
	if (res != TEE_SUCCESS) {
		EMSG("Failed to allocate transient object");
		sess->key_handle = TEE_HANDLE_NULL;
		goto err;
	}

	/*
	 * When loading a key in the cipher session, set_aes_key()
	 * will reset the operation and load a key. But we cannot
	 * reset and operation that has no key yet (GPD TEE Internal
	 * Core API Specification – Public Release v1.1.1, section
	 * 6.2.5 TEE_ResetOperation). In consequence, we will load a
	 * dummy key in the operation so that operation can be reset
	 * when updating the key.
	 */
	key = TEE_Malloc(sess->key_size, 0);
	if (!key) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto err;
	}

	TEE_InitRefAttribute(&attr, TEE_ATTR_SECRET_VALUE, key, sess->key_size);

	res = TEE_PopulateTransientObject(sess->key_handle, &attr, 1);
	if (res != TEE_SUCCESS) {
		EMSG("TEE_PopulateTransientObject failed, %x", res);
		goto err;
	}

	res = TEE_SetOperationKey(sess->op_handle, sess->key_handle);
	if (res != TEE_SUCCESS) {
		EMSG("TEE_SetOperationKey failed %x", res);
		goto err;
	}

	set_aes_key(sess);

	return res;

err:
	if (sess->op_handle != TEE_HANDLE_NULL)
		TEE_FreeOperation(sess->op_handle);
	sess->op_handle = TEE_HANDLE_NULL;

	if (sess->key_handle != TEE_HANDLE_NULL)
		TEE_FreeTransientObject(sess->key_handle);
	sess->key_handle = TEE_HANDLE_NULL;

	return res;
}

/* 
 * called upon command invocation, calls the correct function for the
 * desired command.
 */
TEE_Result TA_InvokeCommandEntryPoint(void *session,
					uint32_t cmd,
					uint32_t param_types,
					TEE_Param params[4])
{
	switch (cmd) {
	case TA_AES_CMD_PREPARE:
		DMSG("call prepare function");
		return alloc_resources(session, param_types, params);
	case TA_AES_CMD_SET_IV:
		DMSG("calling iv function");
		return reset_aes_iv(session, param_types, params);
	case TA_AES_CMD_CIPHER:
		DMSG("calling cipher function");
		return cipher_buffer(session, param_types, params);
	case TA_AES_CMD_WRITE_RAW:
		return create_raw_object(param_types, params);
	default:
		EMSG("Command ID 0x%x is not supported", cmd);
		return TEE_ERROR_NOT_SUPPORTED;
	}
}
