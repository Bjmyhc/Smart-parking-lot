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

#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

/* cJSON 数据类型枚举 */
#define cJSON_False 0			/**< 布尔值 false */
#define cJSON_True 1			/**< 布尔值 true */
#define cJSON_NULL 2			/**< 空值 null */
#define cJSON_Number 3			/**< 数值类型 */
#define cJSON_String 4			/**< 字符串类型 */
#define cJSON_Array 5			/**< 数组类型 */
#define cJSON_Object 6			/**< 对象类型 */
	
#define cJSON_IsReference 256	/**< 引用标记，表示该节点是对其他节点的引用 */
#define cJSON_StringIsConst 512	/**< 字符串常量标记，表示字符串指针不需要释放 */

/**
 * @brief cJSON 节点结构体
 * @note  整个 JSON 树通过该结构体链接而成
 */
typedef struct cJSON {
	struct cJSON *next, *prev;	/**< 后驱/前驱指针，用于遍历数组或对象链表 */
	struct cJSON *child;		/**< 子节点指针，指向数组或对象的首个子元素 */

	int type;					/**< 数据类型，取值为上述 cJSON_XXX 枚举 */

	char *valuestring;			/**< 字符串值，当 type==cJSON_String 时有效 */
	int valueint;				/**< 整型值，当 type==cJSON_Number 时有效 */
	double valuedouble;			/**< 双精度浮点值，当 type==cJSON_Number 时有效 */

	char *string;				/**< 键名，当该节点属于某个对象的子项时保存键名 */
} cJSON;

/**
 * @brief 内存管理钩子结构体
 * @note  用于自定义 cJSON 的内存分配和释放函数
 */
typedef struct cJSON_Hooks {
      void *(*malloc_fn)(size_t sz);	/**< 自定义内存分配函数 */
      void (*free_fn)(void *ptr);		/**< 自定义内存释放函数 */
} cJSON_Hooks;

/**
 * @brief 初始化内存管理钩子
 * @param hooks 钩子结构体指针，传 NULL 则恢复默认 malloc/free
 */
extern void cJSON_InitHooks(cJSON_Hooks* hooks);


/**
 * @brief 解析 JSON 字符串，生成 cJSON 对象树
 * @param value JSON 格式字符串
 * @return 解析成功后返回 cJSON 根节点指针，失败返回 NULL
 * @note  使用完毕后需调用 cJSON_Delete 释放内存
 */
extern cJSON *cJSON_Parse(const char *value);

/**
 * @brief 将 cJSON 对象格式化为带缩进的 JSON 字符串
 * @param item cJSON 对象指针
 * @return 格式化后的字符串，使用完毕后需调用 free 释放
 */
extern char  *cJSON_Print(cJSON *item);

/**
 * @brief 将 cJSON 对象格式化为无缩进的 JSON 字符串（紧凑格式）
 * @param item cJSON 对象指针
 * @return 格式化后的字符串，使用完毕后需调用 free 释放
 */
extern char  *cJSON_PrintUnformatted(cJSON *item);

/**
 * @brief 使用缓冲区策略将 cJSON 对象格式化为字符串
 * @param item       cJSON 对象指针
 * @param prebuffer  预分配的缓冲区大小预估
 * @param fmt        是否格式化：0=紧凑格式，1=带缩进格式
 * @return 格式化后的字符串，使用完毕后需调用 free 释放
 */
extern char *cJSON_PrintBuffered(cJSON *item, int prebuffer, int fmt);

/**
 * @brief 删除 cJSON 对象及其所有子节点
 * @param c 要删除的 cJSON 对象指针
 */
extern void cJSON_Delete(cJSON *c);

/**
 * @brief 获取数组（或对象）的子元素个数
 * @param array cJSON 数组/对象指针
 * @return 元素个数
 */
extern int cJSON_GetArraySize(cJSON *array);

/**
 * @brief 获取数组中指定索引的元素
 * @param array cJSON 数组指针
 * @param item  索引号（从 0 开始）
 * @return 成功返回元素指针，失败返回 NULL
 */
extern cJSON *cJSON_GetArrayItem(cJSON *array, int item);

/**
 * @brief 从对象中根据键名获取元素（不区分大小写）
 * @param object cJSON 对象指针
 * @param string 键名
 * @return 成功返回元素指针，失败返回 NULL
 */
extern cJSON *cJSON_GetObjectItem(cJSON *object, const char *string);

/**
 * @brief 获取解析错误位置指针
 * @return 指向解析出错位置的字符指针
 * @note  当 cJSON_Parse 返回 NULL 时有效，可用于调试
 */
extern const char *cJSON_GetErrorPtr(void);
	
/**
 * @brief 创建 cJSON 基本类型节点
 * @{
 */
extern cJSON *cJSON_CreateNull(void);						/**< 创建 null 节点 */
extern cJSON *cJSON_CreateTrue(void);						/**< 创建 true 节点 */
extern cJSON *cJSON_CreateFalse(void);						/**< 创建 false 节点 */
extern cJSON *cJSON_CreateBool(int b);						/**< 创建布尔节点，b非0=true */
extern cJSON *cJSON_CreateNumber(double num);				/**< 创建数值节点 */
extern cJSON *cJSON_CreateString(const char *string);		/**< 创建字符串节点 */
extern cJSON *cJSON_CreateArray(void);						/**< 创建空数组节点 */
extern cJSON *cJSON_CreateObject(void);						/**< 创建空对象节点 */
/** @} */

/**
 * @brief 批量创建数组类型节点
 * @{
 */
extern cJSON *cJSON_CreateIntArray(const int *numbers, int count);			/**< 从整型数组创建 */
extern cJSON *cJSON_CreateFloatArray(const float *numbers, int count);		/**< 从浮点数组创建 */
extern cJSON *cJSON_CreateDoubleArray(const double *numbers, int count);	/**< 从双精度数组创建 */
extern cJSON *cJSON_CreateStringArray(const char **strings, int count);		/**< 从字符串数组创建 */
/** @} */

/**
 * @brief 向数组/对象中添加元素
 * @{
 */
extern void cJSON_AddItemToArray(cJSON *array, cJSON *item);				/**< 向数组追加元素 */
extern void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);	/**< 向对象添加键值对 */
extern void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);	/**< 向对象添加键值对（字符串为常量，不复制） */
/** @} */

/**
 * @brief 向数组/对象中添加引用（不复制原节点，仅引用）
 * @{
 */
extern void cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
extern void cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);
/** @} */

/**
 * @brief 从数组/对象中分离/删除元素
 * @{
 */
extern cJSON *cJSON_DetachItemFromArray(cJSON *array, int which);		/**< 从数组分离元素（不释放） */
extern void   cJSON_DeleteItemFromArray(cJSON *array, int which);		/**< 从数组删除元素（释放） */
extern cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string);	/**< 从对象分离元素 */
extern void   cJSON_DeleteItemFromObject(cJSON *object, const char *string);	/**< 从对象删除元素 */
/** @} */
	
/**
 * @brief 更新/替换数组/对象中的元素
 * @{
 */
extern void cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);	/**< 在指定位置插入元素，后续元素右移 */
extern void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);	/**< 替换指定位置元素 */
extern void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);	/**< 替换对象中指定键的值 */
/** @} */

/**
 * @brief 深度复制 cJSON 节点
 * @param item   要复制的节点
 * @param recurse 是否递归复制子节点（非0=递归）
 * @return 复制后的新节点指针
 * @note  新节点需要手动调用 cJSON_Delete 释放
 */
extern cJSON *cJSON_Duplicate(cJSON *item, int recurse);

/**
 * @brief 带选项解析 JSON 字符串
 * @param value                   JSON 字符串
 * @param return_parse_end        可选，返回解析结束位置
 * @param require_null_terminated 是否要求 JSON 字符串必须以 null 结尾
 * @return 解析成功返回 cJSON 根节点，失败返回 NULL
 */
extern cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, int require_null_terminated);

/**
 * @brief 去除 JSON 字符串中的空白字符（空格、制表、换行、注释）
 * @param json 传入可修改的 JSON 字符串（原地压缩）
 */
extern void cJSON_Minify(char *json);

/**
 * @brief 快捷宏：向对象中添加各种类型的值
 * @{
 */
#define cJSON_AddNullToObject(object,name)		cJSON_AddItemToObject(object, name, cJSON_CreateNull())
#define cJSON_AddTrueToObject(object,name)		cJSON_AddItemToObject(object, name, cJSON_CreateTrue())
#define cJSON_AddFalseToObject(object,name)		cJSON_AddItemToObject(object, name, cJSON_CreateFalse())
#define cJSON_AddBoolToObject(object,name,b)	cJSON_AddItemToObject(object, name, cJSON_CreateBool(b))
#define cJSON_AddNumberToObject(object,name,n)	cJSON_AddItemToObject(object, name, cJSON_CreateNumber(n))
#define cJSON_AddStringToObject(object,name,s)	cJSON_AddItemToObject(object, name, cJSON_CreateString(s))
/** @} */

/**
 * @brief 设置节点的整型/数值（同时更新 valueint 和 valuedouble）
 * @{
 */
#define cJSON_SetIntValue(object,val)			((object)?(object)->valueint=(object)->valuedouble=(val):(val))
#define cJSON_SetNumberValue(object,val)		((object)?(object)->valueint=(object)->valuedouble=(val):(val))
/** @} */

#ifdef __cplusplus
}
#endif

#endif
