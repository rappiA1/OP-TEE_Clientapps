#ifndef __EEPROMENC_TA_H
#define __EEPROMENC_TA_H

/* UUID of the AES example trusted application */
#define TA_EEENC_UUID \
	{ 0x3f1519ad, 0xab11, 0x4e5d, \
		{ 0xb5, 0x9d, 0xcb, 0xf0, 0x3e, 0xa0, 0x0d, 0xe6 } }
/*
 * TA_AES_CMD_PREPARE - Allocate resources for the AES ciphering
 * param[0] (value) a: TA_AES_ALGO_xxx, b: unused
 * param[1] (value) a: key size in bytes, b: unused
 * param[2] (value) a: TA_AES_MODE_ENCODE/_DECODE, b: unused
 * param[3] unused
 */
#define TA_AES_CMD_PREPARE              0

#define TA_AES_ALGO_ECB                 0
#define TA_AES_ALGO_CBC                 1
#define TA_AES_ALGO_CTR                 2

#define TA_AES_SIZE_128BIT              (128 / 8)
#define TA_AES_SIZE_256BIT              (256 / 8)

#define TA_AES_MODE_ENCODE              1
#define TA_AES_MODE_DECODE              0

/*
 * TA_AES_CMD_SET_KEY - Allocate resources for the AES ciphering
 * param[0] (memref) key data, size shall equal key length
 * param[1] unused
 * param[2] unused
 * param[3] unused
 */
#define TA_AES_CMD_SET_KEY              1

/*
 * TA_AES_CMD_SET_IV - reset IV
 * param[0] (memref) initial vector, size shall equal block length
 * param[1] unused
 * param[2] unused
 * param[3] unused
 */
#define TA_AES_CMD_SET_IV               2

/*
 * TA_AES_CMD_CIPHER - Cipher input buffer into output buffer
 * param[0] (memref) input buffer
 * param[1] (memref) output buffer (shall be bigger than input buffer)
 * param[2] unused
 * param[3] unused
 */
#define TA_AES_CMD_CIPHER               3

#endif /*__EEPROMENC_TA_H */
