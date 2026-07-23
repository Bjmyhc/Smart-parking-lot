/*
  Copyright (c) 2009 Dave Gamble

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* C 语言 JSON 解析器 */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include "cJSON.h"

static const char *ep;	/**< 解析错误位置指针 */

/**
 * @brief 获取解析错误位置
 */
const char *cJSON_GetErrorPtr(void) {return ep;}

/**
 * @brief 字符串比较（不区分大小写）
 */
static int cJSON_strcasecmp(const char *s1, const char *s2)
{
	if (!s1) return (s1==s2)?0:1;
	if (!s2) return 1;
	for(; tolower(*s1) == tolower(*s2); ++s1, ++s2)
		if(*s1 == 0) return 0;
	return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
}

static void *(*cJSON_malloc)(size_t sz) = malloc;	/**< 自定义内存分配函数 */
static void (*cJSON_free)(void *ptr) = free;			/**< 自定义内存释放函数 */

/**
 * @brief 字符串复制（使用自定义内存分配）
 */
static char* cJSON_strdup(const char* str)
{
	size_t len;
	char* copy;

	len = strlen(str) + 1;
	if (!(copy = (char*)cJSON_malloc(len))) return 0;
	memcpy(copy, str, len);
	return copy;
}

/**
 * @brief 初始化内存管理钩子
 * @param hooks 钩子结构体，传 NULL 则恢复默认 malloc/free
 */
void cJSON_InitHooks(cJSON_Hooks* hooks)
{
	if (!hooks) {
		cJSON_malloc = malloc;
		cJSON_free = free;
		return;
	}

	cJSON_malloc = (hooks->malloc_fn) ? hooks->malloc_fn : malloc;
	cJSON_free   = (hooks->free_fn)   ? hooks->free_fn   : free;
}

/**
 * @brief 内部函数：创建新的 cJSON 节点
 */
static cJSON *cJSON_New_Item(void)
{
	cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
	if (node) memset(node, 0, sizeof(cJSON));
	return node;
}

/**
 * @brief 删除 cJSON 对象及其所有子节点（递归释放）
 * @param c 要删除的 cJSON 对象
 */
void cJSON_Delete(cJSON *c)
{
	cJSON *next;
	while (c)
	{
		next=c->next;
		if (!(c->type&cJSON_IsReference) && c->child) cJSON_Delete(c->child);
		if (!(c->type&cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
		if (!(c->type&cJSON_StringIsConst) && c->string) cJSON_free(c->string);
		cJSON_free(c);
		c=next;
	}
}

/**
 * @brief 解析 JSON 数字字符串，填充到 cJSON 节点
 * @param item 目标节点
 * @param num  数字字符串起始位置
 * @return 解析结束后的字符指针
 */
static const char *parse_number(cJSON *item, const char *num)
{
	double n = 0, sign = 1, scale = 0;
	int subscale = 0, signsubscale = 1;

	if (*num == '-') sign = -1, num++;			/* 是否有符号 */
	if (*num == '0') num++;						/* 是否为零 */
	if (*num >= '1' && *num <= '9')
		do	n = (n * 10.0) + (*num++ - '0'); while (*num >= '0' && *num <= '9');	/* 整数部分 */
	if (*num == '.' && num[1] >= '0' && num[1] <= '9') {
		num++;
		do	n = (n * 10.0) + (*num++ - '0'), scale--; while (*num >= '0' && *num <= '9');	/* 小数部分 */
	}
	if (*num == 'e' || *num == 'E')				/* 指数部分 */
	{
		num++;
		if (*num == '+') num++;
		else if (*num == '-') signsubscale = -1, num++;	/* 指数符号 */
		while (*num >= '0' && *num <= '9')
			subscale = (subscale * 10) + (*num++ - '0');	/* 指数数值 */
	}

	/* 计算最终值: +/- number.fraction * 10^+/- exponent */
	n = sign * n * pow(10.0, (scale + subscale * signsubscale));

	item->valuedouble = n;
	item->valueint = (int)n;
	item->type = cJSON_Number;
	return num;
}

/**
 * @brief 计算大于 x 的最小 2 的幂
 */
static int pow2gt(int x) { --x; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; return x + 1; }

/**
 * @brief 打印缓冲区结构体（用于优化字符串输出时的内存分配）
 */
typedef struct {
	char *buffer;	/**< 缓冲区指针 */
	int  length;	/**< 缓冲区总大小 */
	int  offset;	/**< 当前写入偏移 */
} printbuffer;

/**
 * @brief 确保缓冲区有足够空间，不足时自动扩容
 * @param p       打印缓冲区
 * @param needed  需要的字节数
 * @return 可用的缓冲区指针
 */
static char* ensure(printbuffer *p, int needed)
{
	char *newbuffer;
	int newsize;

	if (!p || !p->buffer) return 0;
	needed += p->offset;
	if (needed <= p->length) return p->buffer + p->offset;

	newsize = pow2gt(needed);
	newbuffer = (char*)cJSON_malloc(newsize);
	if (!newbuffer) { cJSON_free(p->buffer); p->length = 0, p->buffer = 0; return 0; }
	if (newbuffer) memcpy(newbuffer, p->buffer, p->length);
	cJSON_free(p->buffer);
	p->length = newsize;
	p->buffer = newbuffer;
	return newbuffer + p->offset;
}

/**
 * @brief 更新缓冲区偏移量
 */
static int update(printbuffer *p)
{
	char *str;
	if (!p || !p->buffer) return 0;
	str = p->buffer + p->offset;
	return p->offset + strlen(str);
}

/**
 * @brief 将数值类型节点格式化为字符串
 * @param item 数值节点
 * @param p    打印缓冲区（可为 NULL）
 * @return 格式化后的字符串
 */
static char *print_number(cJSON *item, printbuffer *p)
{
	char *str = 0;
	double d = item->valuedouble;

	if (d == 0)
	{
		if (p) str = ensure(p, 2);
		else   str = (char*)cJSON_malloc(2);	/* 0 的特殊情况 */
		if (str) strcpy(str, "0");
	}
	else if (fabs(((double)item->valueint) - d) <= DBL_EPSILON && d <= INT_MAX && d >= INT_MIN)
	{
		if (p) str = ensure(p, 21);
		else   str = (char*)cJSON_malloc(21);	/* 2^64+1 最多需要 21 个字符 */
		if (str) sprintf(str, "%d", item->valueint);
	}
	else
	{
		if (p) str = ensure(p, 64);
		else   str = (char*)cJSON_malloc(64);
		if (str)
		{
			if (fabs(floor(d) - d) <= DBL_EPSILON && fabs(d) < 1.0e60)
				sprintf(str, "%.0f", d);
			else if (fabs(d) < 1.0e-6 || fabs(d) > 1.0e9)
				sprintf(str, "%e", d);
			else
				sprintf(str, "%f", d);
		}
	}
	return str;
}

/**
 * @brief 解析 4 位十六进制字符（用于 Unicode 转义）
 */
static unsigned parse_hex4(const char *str)
{
	unsigned h = 0;
	if      (*str >= '0' && *str <= '9') h += (*str) - '0';
	else if (*str >= 'A' && *str <= 'F') h += 10 + (*str) - 'A';
	else if (*str >= 'a' && *str <= 'f') h += 10 + (*str) - 'a';
	else return 0;
	h = h << 4; str++;
	if      (*str >= '0' && *str <= '9') h += (*str) - '0';
	else if (*str >= 'A' && *str <= 'F') h += 10 + (*str) - 'A';
	else if (*str >= 'a' && *str <= 'f') h += 10 + (*str) - 'a';
	else return 0;
	h = h << 4; str++;
	if      (*str >= '0' && *str <= '9') h += (*str) - '0';
	else if (*str >= 'A' && *str <= 'F') h += 10 + (*str) - 'A';
	else if (*str >= 'a' && *str <= 'f') h += 10 + (*str) - 'a';
	else return 0;
	h = h << 4; str++;
	if      (*str >= '0' && *str <= '9') h += (*str) - '0';
	else if (*str >= 'A' && *str <= 'F') h += 10 + (*str) - 'A';
	else if (*str >= 'a' && *str <= 'f') h += 10 + (*str) - 'a';
	else return 0;
	return h;
}

/* UTF-8 首字节标记表 */
static const unsigned char firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

/**
 * @brief 解析 JSON 字符串（处理转义字符和 Unicode）
 * @param item 目标节点
 * @param str  字符串起始位置（含引号）
 * @return 解析结束后的字符指针
 */
static const char *parse_string(cJSON *item, const char *str)
{
	const char *ptr = str + 1;
	char *ptr2;
	char *out;
	int len = 0;
	unsigned uc, uc2;

	if (*str != '\"') { ep = str; return 0; }	/* 不是字符串 */

	/* 跳过转义字符，计算字符串长度 */
	while (*ptr != '\"' && *ptr && ++len)
		if (*ptr++ == '\\') ptr++;

	out = (char*)cJSON_malloc(len + 1);	/* 分配内存 */
	if (!out) return 0;

	ptr = str + 1;
	ptr2 = out;
	while (*ptr != '\"' && *ptr)
	{
		if (*ptr != '\\')
			*ptr2++ = *ptr++;
		else
		{
			ptr++;
			switch (*ptr)
			{
				case 'b': *ptr2++ = '\b'; break;	/* 退格 */
				case 'f': *ptr2++ = '\f'; break;	/* 换页 */
				case 'n': *ptr2++ = '\n'; break;	/* 换行 */
				case 'r': *ptr2++ = '\r'; break;	/* 回车 */
				case 't': *ptr2++ = '\t'; break;	/* 制表符 */
				case 'u':	/* 将 UTF-16 转码为 UTF-8 */
					uc = parse_hex4(ptr + 1);
					ptr += 4;	/* 读取 Unicode 字符 */

					if ((uc >= 0xDC00 && uc <= 0xDFFF) || uc == 0) break;	/* 无效字符 */

					if (uc >= 0xD800 && uc <= 0xDBFF)	/* UTF-16 代理对 */
					{
						if (ptr[1] != '\\' || ptr[2] != 'u') break;	/* 缺少代理对后半部分 */
						uc2 = parse_hex4(ptr + 3);
						ptr += 6;
						if (uc2 < 0xDC00 || uc2 > 0xDFFF) break;	/* 代理对后半部分无效 */
						uc = 0x10000 + (((uc & 0x3FF) << 10) | (uc2 & 0x3FF));
					}

					len = 4;
					if (uc < 0x80)      len = 1;
					else if (uc < 0x800) len = 2;
					else if (uc < 0x10000) len = 3;
					ptr2 += len;

					switch (len) {
						case 4: *--ptr2 = ((uc | 0x80) & 0xBF); uc >>= 6;
						case 3: *--ptr2 = ((uc | 0x80) & 0xBF); uc >>= 6;
						case 2: *--ptr2 = ((uc | 0x80) & 0xBF); uc >>= 6;
						case 1: *--ptr2 = (uc | firstByteMark[len]);
					}
					ptr2 += len;
					break;
				default:  *ptr2++ = *ptr; break;
			}
			ptr++;
		}
	}
	*ptr2 = 0;
	if (*ptr == '\"') ptr++;
	item->valuestring = out;
	item->type = cJSON_String;
	return ptr;
}

/**
 * @brief 将字符串转义为可打印的 JSON 格式
 * @param str 原始字符串
 * @param p   打印缓冲区（可为 NULL）
 * @return 转义后的字符串（含引号）
 */
static char *print_string_ptr(const char *str, printbuffer *p)
{
	const char *ptr;
	char *ptr2, *out;
	int len = 0, flag = 0;
	unsigned char token;

	/* 检查是否需要转义 */
	for (ptr = str; *ptr; ptr++)
		flag |= ((*ptr > 0 && *ptr < 32) || (*ptr == '\"') || (*ptr == '\\')) ? 1 : 0;

	if (!flag)	/* 不需要转义，直接复制 */
	{
		len = ptr - str;
		if (p) out = ensure(p, len + 3);
		else   out = (char*)cJSON_malloc(len + 3);
		if (!out) return 0;
		ptr2 = out;
		*ptr2++ = '\"';
		strcpy(ptr2, str);
		ptr2[len] = '\"';
		ptr2[len + 1] = 0;
		return out;
	}

	if (!str)	/* 空字符串 */
	{
		if (p) out = ensure(p, 3);
		else   out = (char*)cJSON_malloc(3);
		if (!out) return 0;
		strcpy(out, "\"\"");
		return out;
	}

	/* 计算转义后需要的长度 */
	ptr = str;
	while ((token = *ptr) && ++len) {
		if (strchr("\"\\\b\f\n\r\t", token)) len++;
		else if (token < 32) len += 5;
		ptr++;
	}

	if (p) out = ensure(p, len + 3);
	else   out = (char*)cJSON_malloc(len + 3);
	if (!out) return 0;

	ptr2 = out;
	ptr = str;
	*ptr2++ = '\"';
	while (*ptr)
	{
		if ((unsigned char)*ptr > 31 && *ptr != '\"' && *ptr != '\\')
			*ptr2++ = *ptr++;
		else
		{
			*ptr2++ = '\\';
			switch (token = *ptr++)
			{
				case '\\':	*ptr2++ = '\\';	break;
				case '\"':	*ptr2++ = '\"';	break;
				case '\b':	*ptr2++ = 'b';	break;
				case '\f':	*ptr2++ = 'f';	break;
				case '\n':	*ptr2++ = 'n';	break;
				case '\r':	*ptr2++ = 'r';	break;
				case '\t':	*ptr2++ = 't';	break;
				default: sprintf(ptr2, "u%04x", token); ptr2 += 5; break;	/* 转义并输出 */
			}
		}
	}
	*ptr2++ = '\"';
	*ptr2++ = 0;
	return out;
}

/**
 * @brief 调用 print_string_ptr 格式化 cJSON 字符串节点
 */
static char *print_string(cJSON *item, printbuffer *p)
{
	return print_string_ptr(item->valuestring, p);
}

/* 前置函数声明 */
static const char *parse_value(cJSON *item, const char *value);
static char *print_value(cJSON *item, int depth, int fmt, printbuffer *p);
static const char *parse_array(cJSON *item, const char *value);
static char *print_array(cJSON *item, int depth, int fmt, printbuffer *p);
static const char *parse_object(cJSON *item, const char *value);
static char *print_object(cJSON *item, int depth, int fmt, printbuffer *p);

/**
 * @brief 跳过空白字符（空格、回车、换行、制表）
 */
static const char *skip(const char *in)
{
	while (in && *in && (unsigned char)*in <= 32) in++;
	return in;
}

/**
 * @brief 带选项解析 JSON 字符串
 * @param value                   JSON 字符串
 * @param return_parse_end        可选，返回解析结束位置
 * @param require_null_terminated 是否要求 JSON 以 null 结尾（无多余字符）
 * @return 解析成功返回 cJSON 根节点，失败返回 NULL
 */
cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, int require_null_terminated)
{
	const char *end = 0;
	cJSON *c = cJSON_New_Item();
	ep = 0;
	if (!c) return 0;       /* 内存分配失败 */

	end = parse_value(c, skip(value));
	if (!end) { cJSON_Delete(c); return 0; }	/* 解析失败，ep 已设置 */

	/* 如果要求 null 结尾且无多余字符，则跳过空白后检查 */
	if (require_null_terminated) {
		end = skip(end);
		if (*end) { cJSON_Delete(c); ep = end; return 0; }
	}
	if (return_parse_end) *return_parse_end = end;
	return c;
}

/**
 * @brief 默认方式解析 JSON 字符串（不要求 null 结尾，不返回结束位置）
 */
cJSON *cJSON_Parse(const char *value)
{
	return cJSON_ParseWithOpts(value, 0, 0);
}

/**
 * @brief 将 cJSON 对象格式化为带缩进的 JSON 字符串
 */
char *cJSON_Print(cJSON *item)
{
	return print_value(item, 0, 1, 0);
}

/**
 * @brief 将 cJSON 对象格式化为无缩进的紧凑 JSON 字符串
 */
char *cJSON_PrintUnformatted(cJSON *item)
{
	return print_value(item, 0, 0, 0);
}

/**
 * @brief 使用缓冲区策略格式化 cJSON 对象
 * @param item       cJSON 对象
 * @param prebuffer  预分配缓冲区大小
 * @param fmt        是否格式化（0=紧凑，1=缩进）
 */
char *cJSON_PrintBuffered(cJSON *item, int prebuffer, int fmt)
{
	printbuffer p;
	p.buffer = (char*)cJSON_malloc(prebuffer);
	p.length = prebuffer;
	p.offset = 0;
	return print_value(item, 0, fmt, &p);
	return p.buffer;
}


/**
 * @brief 解析核心：根据值类型调用对应的解析函数
 * @param item  目标节点
 * @param value 待解析的字符串
 * @return 解析结束位置
 */
static const char *parse_value(cJSON *item, const char *value)
{
	if (!value) return 0;	/* 空指针，解析失败 */
	if (!strncmp(value, "null", 4))  { item->type = cJSON_NULL;  return value + 4; }
	if (!strncmp(value, "false", 5)) { item->type = cJSON_False; return value + 5; }
	if (!strncmp(value, "true", 4))  { item->type = cJSON_True;  item->valueint = 1; return value + 4; }
	if (*value == '\"')              { return parse_string(item, value); }
	if (*value == '-' || (*value >= '0' && *value <= '9'))
	                                 { return parse_number(item, value); }
	if (*value == '[')               { return parse_array(item, value); }
	if (*value == '{')               { return parse_object(item, value); }

	ep = value; return 0;	/* 无法识别的字符，解析失败 */
}

/**
 * @brief 将 cJSON 节点值渲染为文本
 * @param item  cJSON 节点
 * @param depth 当前嵌套深度（用于缩进）
 * @param fmt   是否格式化
 * @param p     打印缓冲区
 * @return 渲染后的字符串
 */
static char *print_value(cJSON *item, int depth, int fmt, printbuffer *p)
{
	char *out=0;
	if (!item) return 0;
	if (p)
	{
		switch ((item->type)&255)
		{
			case cJSON_NULL:	{out=ensure(p,5);	if (out) strcpy(out,"null");	break;}
			case cJSON_False:	{out=ensure(p,6);	if (out) strcpy(out,"false");	break;}
			case cJSON_True:	{out=ensure(p,5);	if (out) strcpy(out,"true");	break;}
			case cJSON_Number:	out=print_number(item,p);break;
			case cJSON_String:	out=print_string(item,p);break;
			case cJSON_Array:	out=print_array(item,depth,fmt,p);break;
			case cJSON_Object:	out=print_object(item,depth,fmt,p);break;
		}
	}
	else
	{
		switch ((item->type)&255)
		{
			case cJSON_NULL:	out=cJSON_strdup("null");	break;
			case cJSON_False:	out=cJSON_strdup("false");break;
			case cJSON_True:	out=cJSON_strdup("true"); break;
			case cJSON_Number:	out=print_number(item,0);break;
			case cJSON_String:	out=print_string(item,0);break;
			case cJSON_Array:	out=print_array(item,depth,fmt,0);break;
			case cJSON_Object:	out=print_object(item,depth,fmt,0);break;
		}
	}
	return out;
}

/**
 * @brief 解析 JSON 数组（[...]）
 * @param item  目标节点
 * @param value 待解析字符串
 * @return 解析结束位置
 */
static const char *parse_array(cJSON *item, const char *value)
{
	cJSON *child;

	if (*value != '[') { ep = value; return 0; }	/* 不是数组 */

	item->type = cJSON_Array;
	value = skip(value + 1);
	if (*value == ']') return value + 1;	/* 空数组 */

	item->child = child = cJSON_New_Item();
	if (!item->child) return 0;				/* 内存分配失败 */
	value = skip(parse_value(child, skip(value)));	/* 跳过空白，解析元素值 */
	if (!value) return 0;

	while (*value == ',')
	{
		cJSON *new_item;
		if (!(new_item = cJSON_New_Item())) return 0; 	/* 内存分配失败 */
		child->next = new_item;
		new_item->prev = child;
		child = new_item;
		value = skip(parse_value(child, skip(value + 1)));
		if (!value) return 0;
	}

	if (*value == ']') return value + 1;	/* 数组结束 */
	ep = value; return 0;					/* 格式错误 */
}

/**
 * @brief 将 cJSON 数组渲染为文本
 * @param item  数组节点
 * @param depth 嵌套深度
 * @param fmt   是否格式化
 * @param p     打印缓冲区
 * @return 渲染后的字符串
 */
static char *print_array(cJSON *item, int depth, int fmt, printbuffer *p)
{
	char **entries;
	char *out=0,*ptr,*ret;int len=5;
	cJSON *child=item->child;
	int numentries=0,i=0,fail=0;
	size_t tmplen=0;
	
	/* How many entries in the array? */
	while (child) numentries++,child=child->next;
	/* Explicitly handle numentries==0 */
	if (!numentries)
	{
		if (p)	out=ensure(p,3);
		else	out=(char*)cJSON_malloc(3);
		if (out) strcpy(out,"[]");
		return out;
	}

	if (p)
	{
		/* Compose the output array. */
		i=p->offset;
		ptr=ensure(p,1);if (!ptr) return 0;	*ptr='[';	p->offset++;
		child=item->child;
		while (child && !fail)
		{
			print_value(child,depth+1,fmt,p);
			p->offset=update(p);
			if (child->next) {len=fmt?2:1;ptr=ensure(p,len+1);if (!ptr) return 0;*ptr++=',';if(fmt)*ptr++=' ';*ptr=0;p->offset+=len;}
			child=child->next;
		}
		ptr=ensure(p,2);if (!ptr) return 0;	*ptr++=']';*ptr=0;
		out=(p->buffer)+i;
	}
	else
	{
		/* Allocate an array to hold the values for each */
		entries=(char**)cJSON_malloc(numentries*sizeof(char*));
		if (!entries) return 0;
		memset(entries,0,numentries*sizeof(char*));
		/* Retrieve all the results: */
		child=item->child;
		while (child && !fail)
		{
			ret=print_value(child,depth+1,fmt,0);
			entries[i++]=ret;
			if (ret) len+=strlen(ret)+2+(fmt?1:0); else fail=1;
			child=child->next;
		}
		
		/* If we didn't fail, try to malloc the output string */
		if (!fail)	out=(char*)cJSON_malloc(len);
		/* If that fails, we fail. */
		if (!out) fail=1;

		/* Handle failure. */
		if (fail)
		{
			for (i=0;i<numentries;i++) if (entries[i]) cJSON_free(entries[i]);
			cJSON_free(entries);
			return 0;
		}
		
		/* Compose the output array. */
		*out='[';
		ptr=out+1;*ptr=0;
		for (i=0;i<numentries;i++)
		{
			tmplen=strlen(entries[i]);memcpy(ptr,entries[i],tmplen);ptr+=tmplen;
			if (i!=numentries-1) {*ptr++=',';if(fmt)*ptr++=' ';*ptr=0;}
			cJSON_free(entries[i]);
		}
		cJSON_free(entries);
		*ptr++=']';*ptr++=0;
	}
	return out;	
}

/**
 * @brief 解析 JSON 对象（{...}）
 * @param item  目标节点
 * @param value 待解析字符串
 * @return 解析结束位置
 */
static const char *parse_object(cJSON *item, const char *value)
{
	cJSON *child;

	if (*value != '{') { ep = value; return 0; }	/* 不是对象 */

	item->type = cJSON_Object;
	value = skip(value + 1);
	if (*value == '}') return value + 1;	/* 空对象 */

	item->child = child = cJSON_New_Item();
	if (!item->child) return 0;
	value = skip(parse_string(child, skip(value)));
	if (!value) return 0;
	child->string = child->valuestring;
	child->valuestring = 0;
	if (*value != ':') { ep = value; return 0; }	/* 缺少冒号 */
	value = skip(parse_value(child, skip(value + 1)));	/* 解析值 */
	if (!value) return 0;

	while (*value == ',')
	{
		cJSON *new_item;
		if (!(new_item = cJSON_New_Item())) return 0; /* 内存分配失败 */
		child->next = new_item;
		new_item->prev = child;
		child = new_item;
		value = skip(parse_string(child, skip(value + 1)));
		if (!value) return 0;
		child->string = child->valuestring;
		child->valuestring = 0;
		if (*value != ':') { ep = value; return 0; }	/* 缺少冒号 */
		value = skip(parse_value(child, skip(value + 1)));
		if (!value) return 0;
	}

	if (*value == '}') return value + 1;	/* 对象结束 */
	ep = value; return 0;					/* 格式错误 */
}

/**
 * @brief 将 cJSON 对象渲染为文本
 * @param item  对象节点
 * @param depth 嵌套深度
 * @param fmt   是否格式化
 * @param p     打印缓冲区
 * @return 渲染后的字符串
 */
static char *print_object(cJSON *item, int depth, int fmt, printbuffer *p)
{
	char **entries=0,**names=0;
	char *out=0,*ptr,*ret,*str;int len=7,i=0,j;
	cJSON *child=item->child;
	int numentries=0,fail=0;
	size_t tmplen=0;
	/* Count the number of entries. */
	while (child) numentries++,child=child->next;
	/* Explicitly handle empty object case */
	if (!numentries)
	{
		if (p) out=ensure(p,fmt?depth+4:3);
		else	out=(char*)cJSON_malloc(fmt?depth+4:3);
		if (!out)	return 0;
		ptr=out;*ptr++='{';
		if (fmt) {*ptr++='\n';for (i=0;i<depth-1;i++) *ptr++='\t';}
		*ptr++='}';*ptr++=0;
		return out;
	}
	if (p)
	{
		/* Compose the output: */
		i=p->offset;
		len=fmt?2:1;	ptr=ensure(p,len+1);	if (!ptr) return 0;
		*ptr++='{';	if (fmt) *ptr++='\n';	*ptr=0;	p->offset+=len;
		child=item->child;depth++;
		while (child)
		{
			if (fmt)
			{
				ptr=ensure(p,depth);	if (!ptr) return 0;
				for (j=0;j<depth;j++) *ptr++='\t';
				p->offset+=depth;
			}
			print_string_ptr(child->string,p);
			p->offset=update(p);
			
			len=fmt?2:1;
			ptr=ensure(p,len);	if (!ptr) return 0;
			*ptr++=':';if (fmt) *ptr++='\t';
			p->offset+=len;
			
			print_value(child,depth,fmt,p);
			p->offset=update(p);

			len=(fmt?1:0)+(child->next?1:0);
			ptr=ensure(p,len+1); if (!ptr) return 0;
			if (child->next) *ptr++=',';
			if (fmt) *ptr++='\n';*ptr=0;
			p->offset+=len;
			child=child->next;
		}
		ptr=ensure(p,fmt?(depth+1):2);	 if (!ptr) return 0;
		if (fmt)	for (i=0;i<depth-1;i++) *ptr++='\t';
		*ptr++='}';*ptr=0;
		out=(p->buffer)+i;
	}
	else
	{
		/* Allocate space for the names and the objects */
		entries=(char**)cJSON_malloc(numentries*sizeof(char*));
		if (!entries) return 0;
		names=(char**)cJSON_malloc(numentries*sizeof(char*));
		if (!names) {cJSON_free(entries);return 0;}
		memset(entries,0,sizeof(char*)*numentries);
		memset(names,0,sizeof(char*)*numentries);

		/* Collect all the results into our arrays: */
		child=item->child;depth++;if (fmt) len+=depth;
		while (child)
		{
			names[i]=str=print_string_ptr(child->string,0);
			entries[i++]=ret=print_value(child,depth,fmt,0);
			if (str && ret) len+=strlen(ret)+strlen(str)+2+(fmt?2+depth:0); else fail=1;
			child=child->next;
		}
		
		/* Try to allocate the output string */
		if (!fail)	out=(char*)cJSON_malloc(len);
		if (!out) fail=1;

		/* Handle failure */
		if (fail)
		{
			for (i=0;i<numentries;i++) {if (names[i]) cJSON_free(names[i]);if (entries[i]) cJSON_free(entries[i]);}
			cJSON_free(names);cJSON_free(entries);
			return 0;
		}
		
		/* Compose the output: */
		*out='{';ptr=out+1;if (fmt)*ptr++='\n';*ptr=0;
		for (i=0;i<numentries;i++)
		{
			if (fmt) for (j=0;j<depth;j++) *ptr++='\t';
			tmplen=strlen(names[i]);memcpy(ptr,names[i],tmplen);ptr+=tmplen;
			*ptr++=':';if (fmt) *ptr++='\t';
			strcpy(ptr,entries[i]);ptr+=strlen(entries[i]);
			if (i!=numentries-1) *ptr++=',';
			if (fmt) *ptr++='\n';*ptr=0;
			cJSON_free(names[i]);cJSON_free(entries[i]);
		}
		
		cJSON_free(names);cJSON_free(entries);
		if (fmt) for (i=0;i<depth-1;i++) *ptr++='\t';
		*ptr++='}';*ptr++=0;
	}
	return out;	
}

/**
 * @brief 获取数组（或对象）中元素个数
 * @param array cJSON 数组/对象节点
 * @return 元素个数
 */
int cJSON_GetArraySize(cJSON *array)
{
	cJSON *c = array->child;
	int i = 0;
	while (c) i++, c = c->next;
	return i;
}

/**
 * @brief 获取数组中指定索引的元素
 * @param array cJSON 数组节点
 * @param item  索引号（从 0 开始）
 * @return 成功返回元素指针，失败返回 NULL
 */
cJSON *cJSON_GetArrayItem(cJSON *array, int item)
{
	cJSON *c = array->child;
	while (c && item > 0) item--, c = c->next;
	return c;
}

/**
 * @brief 根据键名获取对象中的元素（不区分大小写）
 * @param object cJSON 对象节点
 * @param string 键名
 * @return 成功返回元素指针，失败返回 NULL
 */
cJSON *cJSON_GetObjectItem(cJSON *object, const char *string)
{
	cJSON *c = object->child;
	while (c && cJSON_strcasecmp(c->string, string))
		c = c->next;
	return c;
}

/**
 * @brief 连接两个 cJSON 节点（设置链表关系）
 * @param prev 前一个节点
 * @param item 当前节点
 */
static void suffix_object(cJSON *prev, cJSON *item)
{
	prev->next = item;
	item->prev = prev;
}

/**
 * @brief 创建 cJSON 节点的引用（不复制数据，仅引用）
 * @param item 原节点
 * @return 新的引用节点
 */
static cJSON *create_reference(cJSON *item)
{
	cJSON *ref = cJSON_New_Item();
	if (!ref) return 0;
	memcpy(ref, item, sizeof(cJSON));
	ref->string = 0;
	ref->type |= cJSON_IsReference;
	ref->next = ref->prev = 0;
	return ref;
}

/**
 * @brief 向数组中添加元素
 * @param array 目标数组
 * @param item  要添加的元素
 */
void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
	cJSON *c = array->child;
	if (!item) return;
	if (!c) {
		array->child = item;
	} else {
		while (c && c->next) c = c->next;
		suffix_object(c, item);
	}
}

/**
 * @brief 向对象中添加键值对
 * @param object 目标对象
 * @param string 键名
 * @param item   值节点
 */
void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
	if (!item) return;
	if (item->string) cJSON_free(item->string);
	item->string = cJSON_strdup(string);
	cJSON_AddItemToArray(object, item);
}

/**
 * @brief 向对象中添加键值对（字符串为常量，不复制）
 * @param object 目标对象
 * @param string 键名（常量字符串）
 * @param item   值节点
 */
void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
	if (!item) return;
	if (!(item->type & cJSON_StringIsConst) && item->string)
		cJSON_free(item->string);
	item->string = (char*)string;
	item->type |= cJSON_StringIsConst;
	cJSON_AddItemToArray(object, item);
}

/**
 * @brief 向数组中添加元素的引用（不复制原节点）
 * @param array 目标数组
 * @param item  原节点
 */
void cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
	cJSON_AddItemToArray(array, create_reference(item));
}

/**
 * @brief 向对象中添加键值对的引用（不复制原节点）
 * @param object 目标对象
 * @param string 键名
 * @param item   原节点
 */
void cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
	cJSON_AddItemToObject(object, string, create_reference(item));
}

/**
 * @brief 从数组中分离指定索引的元素（不释放内存）
 * @param array 目标数组
 * @param which 索引号
 * @return 分离出的元素节点
 */
cJSON *cJSON_DetachItemFromArray(cJSON *array, int which)
{
	cJSON *c = array->child;
	while (c && which > 0) c = c->next, which--;
	if (!c) return 0;

	if (c->prev) c->prev->next = c->next;
	if (c->next) c->next->prev = c->prev;
	if (c == array->child) array->child = c->next;
	c->prev = c->next = 0;
	return c;
}

/**
 * @brief 从数组中删除指定索引的元素（释放内存）
 * @param array 目标数组
 * @param which 索引号
 */
void cJSON_DeleteItemFromArray(cJSON *array, int which)
{
	cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

/**
 * @brief 从对象中分离指定键名的元素（不释放内存）
 * @param object 目标对象
 * @param string 键名
 * @return 分离出的元素节点
 */
cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
	int i = 0;
	cJSON *c = object->child;
	while (c && cJSON_strcasecmp(c->string, string)) i++, c = c->next;
	if (c) return cJSON_DetachItemFromArray(object, i);
	return 0;
}

/**
 * @brief 从对象中删除指定键名的元素（释放内存）
 * @param object 目标对象
 * @param string 键名
 */
void cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
	cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

/**
 * @brief 在数组指定位置插入新元素
 * @param array    目标数组
 * @param which    插入位置索引
 * @param newitem  新元素节点
 */
void cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
	cJSON *c = array->child;
	while (c && which > 0) c = c->next, which--;
	if (!c) {
		cJSON_AddItemToArray(array, newitem);
		return;
	}

	newitem->next = c;
	newitem->prev = c->prev;
	c->prev = newitem;
	if (c == array->child)
		array->child = newitem;
	else
		newitem->prev->next = newitem;
}

/**
 * @brief 替换数组中指定位置的元素
 * @param array    目标数组
 * @param which    替换位置索引
 * @param newitem  新元素节点
 */
void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
	cJSON *c = array->child;
	while (c && which > 0) c = c->next, which--;
	if (!c) return;

	newitem->next = c->next;
	newitem->prev = c->prev;
	if (newitem->next) newitem->next->prev = newitem;
	if (c == array->child)
		array->child = newitem;
	else
		newitem->prev->next = newitem;
	c->next = c->prev = 0;
	cJSON_Delete(c);
}

/**
 * @brief 替换对象中指定键名的值
 * @param object   目标对象
 * @param string   键名
 * @param newitem  新值节点
 */
void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
	int i = 0;
	cJSON *c = object->child;
	while (c && cJSON_strcasecmp(c->string, string)) i++, c = c->next;
	if (c) {
		newitem->string = cJSON_strdup(string);
		cJSON_ReplaceItemInArray(object, i, newitem);
	}
}

/**
 * @brief 创建 null 类型节点
 */
cJSON *cJSON_CreateNull(void)
{
	cJSON *item = cJSON_New_Item();
	if (item) item->type = cJSON_NULL;
	return item;
}

/**
 * @brief 创建 true 类型节点
 */
cJSON *cJSON_CreateTrue(void)
{
	cJSON *item = cJSON_New_Item();
	if (item) item->type = cJSON_True;
	return item;
}

/**
 * @brief 创建 false 类型节点
 */
cJSON *cJSON_CreateFalse(void)
{
	cJSON *item = cJSON_New_Item();
	if (item) item->type = cJSON_False;
	return item;
}

/**
 * @brief 创建布尔类型节点
 * @param b 布尔值（非0为true）
 */
cJSON *cJSON_CreateBool(int b)
{
	cJSON *item = cJSON_New_Item();
	if (item) item->type = b ? cJSON_True : cJSON_False;
	return item;
}

/**
 * @brief 创建数值类型节点
 * @param num 数值
 */
cJSON *cJSON_CreateNumber(double num)
{
	cJSON *item = cJSON_New_Item();
	if (item) {
		item->type = cJSON_Number;
		item->valuedouble = num;
		item->valueint = (int)num;
	}
	return item;
}

/**
 * @brief 创建字符串类型节点
 * @param string 字符串值
 */
cJSON *cJSON_CreateString(const char *string)
{
	cJSON *item = cJSON_New_Item();
	if (item) {
		item->type = cJSON_String;
		item->valuestring = cJSON_strdup(string);
	}
	return item;
}

/**
 * @brief 创建空数组节点
 */
cJSON *cJSON_CreateArray(void)
{
	cJSON *item = cJSON_New_Item();
	if (item) item->type = cJSON_Array;
	return item;
}

/**
 * @brief 创建空对象节点
 */
cJSON *cJSON_CreateObject(void)
{
	cJSON *item = cJSON_New_Item();
	if (item) item->type = cJSON_Object;
	return item;
}

/**
 * @brief 从整型数组创建 cJSON 数组节点
 * @param numbers 整型数组
 * @param count   元素个数
 */
cJSON *cJSON_CreateIntArray(const int *numbers, int count)
{
	int i;
	cJSON *n = 0, *p = 0, *a = cJSON_CreateArray();
	for (i = 0; a && i < count; i++) {
		n = cJSON_CreateNumber(numbers[i]);
		if (!i)
			a->child = n;
		else
			suffix_object(p, n);
		p = n;
	}
	return a;
}

/**
 * @brief 从浮点数组创建 cJSON 数组节点
 * @param numbers 浮点数组
 * @param count   元素个数
 */
cJSON *cJSON_CreateFloatArray(const float *numbers, int count)
{
	int i;
	cJSON *n = 0, *p = 0, *a = cJSON_CreateArray();
	for (i = 0; a && i < count; i++) {
		n = cJSON_CreateNumber(numbers[i]);
		if (!i)
			a->child = n;
		else
			suffix_object(p, n);
		p = n;
	}
	return a;
}

/**
 * @brief 从双精度数组创建 cJSON 数组节点
 * @param numbers 双精度数组
 * @param count   元素个数
 */
cJSON *cJSON_CreateDoubleArray(const double *numbers, int count)
{
	int i;
	cJSON *n = 0, *p = 0, *a = cJSON_CreateArray();
	for (i = 0; a && i < count; i++) {
		n = cJSON_CreateNumber(numbers[i]);
		if (!i)
			a->child = n;
		else
			suffix_object(p, n);
		p = n;
	}
	return a;
}

/**
 * @brief 从字符串数组创建 cJSON 数组节点
 * @param strings 字符串数组
 * @param count   元素个数
 */
cJSON *cJSON_CreateStringArray(const char **strings, int count)
{
	int i;
	cJSON *n = 0, *p = 0, *a = cJSON_CreateArray();
	for (i = 0; a && i < count; i++) {
		n = cJSON_CreateString(strings[i]);
		if (!i)
			a->child = n;
		else
			suffix_object(p, n);
		p = n;
	}
	return a;
}

/**
 * @brief 深度复制 cJSON 节点
 * @param item    要复制的节点
 * @param recurse 是否递归复制子节点
 * @return 新的 cJSON 节点
 */
cJSON *cJSON_Duplicate(cJSON *item, int recurse)
{
	cJSON *newitem, *cptr, *nptr = 0, *newchild;

	/* 检查输入参数 */
	if (!item) return 0;

	/* 创建新节点 */
	newitem = cJSON_New_Item();
	if (!newitem) return 0;

	/* 复制基本属性 */
	newitem->type = item->type & (~cJSON_IsReference);
	newitem->valueint = item->valueint;
	newitem->valuedouble = item->valuedouble;

	if (item->valuestring) {
		newitem->valuestring = cJSON_strdup(item->valuestring);
		if (!newitem->valuestring) {
			cJSON_Delete(newitem);
			return 0;
		}
	}

	if (item->string) {
		newitem->string = cJSON_strdup(item->string);
		if (!newitem->string) {
			cJSON_Delete(newitem);
			return 0;
		}
	}

	/* 非递归复制则直接返回 */
	if (!recurse) return newitem;

	/* 递归复制子节点 */
	cptr = item->child;
	while (cptr)
	{
		newchild = cJSON_Duplicate(cptr, 1);	/* 递归复制每个子节点 */
		if (!newchild) {
			cJSON_Delete(newitem);
			return 0;
		}

		if (nptr) {
			/* 如果已有子节点，则连接链表 */
			nptr->next = newchild;
			newchild->prev = nptr;
			nptr = newchild;
		} else {
			/* 第一个子节点 */
			newitem->child = newchild;
			nptr = newchild;
		}
		cptr = cptr->next;
	}
	return newitem;
}

/**
 * @brief 压缩 JSON 字符串（去除空白字符和注释）
 * @param json 可修改的 JSON 字符串
 */
void cJSON_Minify(char *json)
{
	char *into = json;
	while (*json)
	{
		if (*json == ' ') json++;
		else if (*json == '\t') json++;	/* 制表符 */
		else if (*json == '\r') json++;
		else if (*json == '\n') json++;
		else if (*json == '/' && json[1] == '/')  while (*json && *json != '\n') json++;	/* 单行注释 */
		else if (*json == '/' && json[1] == '*') {while (*json && !(*json == '*' && json[1] == '/')) json++;json+=2;}	/* 多行注释 */
		else if (*json == '\"'){*into++=*json++;while (*json && *json!='\"'){if (*json=='\\') *into++=*json++;*into++=*json++;}*into++=*json++;} /* 字符串（保留转义） */
		else *into++=*json++;			/* 其他字符 */
	}
	*into=0;	/* 添加结束符 */
}
