/**
* 定义检查字符串中是否含有UTF-8字符的方法
*/
#include "utf-8.h"

#include <stdlib.h>
#include <string.h>


#if !defined(ARRAY_SIZE)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#endif

/**
 * Structure to hold the valid ranges of UTF-8 characters, for each byte up to 4
 */
struct
{
	int len; /**< number of elements in the following array (1 to 4) */
	struct
	{
		char lower; /**< lower limit of valid range */
		char upper; /**< upper limit of valid range */
	} bytes[4];   /**< up to 4 bytes can be used per character */
}
valid_ranges[] = {
		{1, { {00, 0x7F} } },
		{2, { {0xC2, 0xDF}, {0x80, 0xBF} } },
		{3, { {0xE0, 0xE0}, {0xA0, 0xBF}, {0x80, 0xBF} } },
		{3, { {0xE1, 0xEC}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{3, { {0xED, 0xED}, {0x80, 0x9F}, {0x80, 0xBF} } },
		{3, { {0xEE, 0xEF}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{4, { {0xF0, 0xF0}, {0x90, 0xBF}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{4, { {0xF1, 0xF3}, {0x80, 0xBF}, {0x80, 0xBF}, {0x80, 0xBF} } },
		{4, { {0xF4, 0xF4}, {0x80, 0x8F}, {0x80, 0xBF}, {0x80, 0xBF} } },
};

static const char* UTF8_char_validate(int len, const char* data);


/**
 * 检验传入的字符串是否是UTF-8
 * @param len data的长度
 * @param data 要检验的字符串
 * @return pointer to the start of the next UTF-8 character in "data"
 */
static const char* UTF8_char_validate(int len, const char* data)
{
	int good = 0;
	int charlen = 2;
	int i, j;
	const char *rc = NULL;

	/* first work out how many bytes this char is encoded in */
	if ((data[0] & 128) == 0)
		charlen = 1;
	else if ((data[0] & 0xF0) == 0xF0)
		charlen = 4;
	else if ((data[0] & 0xE0) == 0xE0)
		charlen = 3;

	if (charlen > len)
		goto exit;	/* not enough characters in the string we were given */

	for (i = 0; i < ARRAY_SIZE(valid_ranges); ++i){ /* just has to match one of these rows */
		if (valid_ranges[i].len == charlen){
			good = 1;
			for (j = 0; j < charlen; ++j){
				if (data[j] < valid_ranges[i].bytes[j].lower ||
						data[j] > valid_ranges[i].bytes[j].upper){
					good = 0;  /* failed the check */
					break;
				}
			}
			if (good)
				break;
		}
	}

	if (good)
		rc = data + charlen;
	exit:
	return rc;
}


/**
 * 检验传入的字符串是否只含有UTF-8字符
 * @param len data长度
 * @param data 要检验的字符串
 * @return 1 (true) 只包含UTF-8, 0 (false) 含有其他字符
 */
int UTF8_validate(int len, const char* data) {
	const char* curData = NULL;
	int rc = 0;

	if (len == 0)
	{
		rc = 1;
		goto exit;
	}
	curData = UTF8_char_validate(len, data);
	while (curData && (curData < data + len))
		curData = UTF8_char_validate(len, curData);

	rc = curData != NULL;
exit:
	return rc;
}


/**
 * 验证以null结尾的字符串是否只含有UTF-8字符
 * @param 要检验的字符串
 * @return 1 (true) 只包含UTF-8, 0 (false) 含有其他字符
 */
int UTF8_validateString(const char* string) {
	int rc = 0;

	rc = UTF8_validate((int)strlen(string), string);
	return rc;
}
