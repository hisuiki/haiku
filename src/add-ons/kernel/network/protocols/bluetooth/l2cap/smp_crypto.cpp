// Security Manager crypto, per Bluetooth Core spec Vol 3 Part H 2.2.
#include "smp_crypto.h"
#include <string.h>


/*!	Everything the Security Manager exchanges is least significant octet
	first, while AES works most significant octet first.
*/
static void
swap_buffer(const uint8* source, uint8* destination, size_t size)
{
	for (size_t i = 0; i < size; i++)
		destination[size - 1 - i] = source[i];
}


void
smp_e(const uint8 key[16], uint8 data[16])
{
	uint8 swappedKey[16];
	uint8 swappedData[16];
	aes128_context context;

	swap_buffer(key, swappedKey, 16);
	aes128_set_key(&context, swappedKey);

	swap_buffer(data, swappedData, 16);
	aes128_encrypt(&context, swappedData, swappedData);

	swap_buffer(swappedData, data, 16);

	memset(&context, 0, sizeof(context));
	memset(swappedKey, 0, sizeof(swappedKey));
}


void
smp_c1(const uint8 key[16], const uint8 random[16], const uint8 preq[7],
	const uint8 pres[7], uint8 initiatorAddressType,
	const uint8 initiatorAddress[6], uint8 responderAddressType,
	const uint8 responderAddress[6], uint8 result[16])
{
	uint8 p1[16];
	uint8 p2[16];

	// p1 = pres || preq || rat || iat
	p1[0] = initiatorAddressType;
	p1[1] = responderAddressType;
	memcpy(p1 + 2, preq, 7);
	memcpy(p1 + 9, pres, 7);

	for (int i = 0; i < 16; i++)
		result[i] = (uint8)(random[i] ^ p1[i]);

	smp_e(key, result);

	// p2 = padding || ia || ra
	memcpy(p2, responderAddress, 6);
	memcpy(p2 + 6, initiatorAddress, 6);
	memset(p2 + 12, 0, 4);

	for (int i = 0; i < 16; i++)
		result[i] ^= p2[i];

	smp_e(key, result);
}


void
smp_s1(const uint8 key[16], const uint8 random1[16], const uint8 random2[16],
	uint8 result[16])
{
	// Only the least significant half of each random value is used.
	memcpy(result, random2, 8);
	memcpy(result + 8, random1, 8);

	smp_e(key, result);
}
