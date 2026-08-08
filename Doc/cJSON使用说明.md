# cJSON 库使用说明

cJSON 是一个轻量级的 JSON 解析器和生成器，专为嵌入式系统设计，具有占用资源少、使用简单的特点。

## 一、核心数据结构

### cJSON 结构体
```c
typedef struct cJSON {
    struct cJSON *next, *prev;  // 链表指针
    struct cJSON *child;        // 子节点指针
    int type;                   // 数据类型
    char *valuestring;          // 字符串值
    int valueint;               // 整型值
    double valuedouble;         // 双精度浮点值
    char *string;               // 键名
} cJSON;
```

### 数据类型枚举
```c
#define cJSON_False 0     // 布尔值 false
#define cJSON_True 1      // 布尔值 true
#define cJSON_NULL 2      // 空值 null
#define cJSON_Number 3    // 数值类型
#define cJSON_String 4    // 字符串类型
#define cJSON_Array 5     // 数组类型
#define cJSON_Object 6    // 对象类型
```

## 二、核心解析函数

### 1. cJSON_Parse
```c
cJSON *cJSON_Parse(const char *value);
```
- **功能**：解析 JSON 字符串，生成 cJSON 对象树
- **参数**：JSON 格式字符串
- **返回值**：解析成功返回 cJSON 根节点指针，失败返回 NULL
- **使用示例**：
  ```c
  cJSON *root = cJSON_Parse("{\"name\":\"张三\",\"age\":25}");
  if (root) {
      // 处理解析结果
      cJSON_Delete(root);  // 使用完必须释放
  }
  ```

### 2. cJSON_ParseWithOpts
```c
cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, int require_null_terminated);
```
- **功能**：带选项解析 JSON 字符串
- **参数**：
  - `value`：JSON 字符串
  - `return_parse_end`：可选，返回解析结束位置
  - `require_null_terminated`：是否要求 JSON 以 null 结尾
- **返回值**：解析成功返回 cJSON 根节点，失败返回 NULL

## 三、格式化输出函数

### 1. cJSON_Print
```c
char *cJSON_Print(cJSON *item);
```
- **功能**：将 cJSON 对象格式化为带缩进的 JSON 字符串
- **参数**：cJSON 对象指针
- **返回值**：格式化后的字符串，使用完毕后需调用 free 释放

### 2. cJSON_PrintUnformatted
```c
char *cJSON_PrintUnformatted(cJSON *item);
```
- **功能**：将 cJSON 对象格式化为无缩进的紧凑 JSON 字符串
- **参数**：cJSON 对象指针
- **返回值**：格式化后的字符串，使用完毕后需调用 free 释放

### 3. cJSON_PrintBuffered
```c
char *cJSON_PrintBuffered(cJSON *item, int prebuffer, int fmt);
```
- **功能**：使用缓冲区策略格式化 cJSON 对象
- **参数**：
  - `item`：cJSON 对象
  - `prebuffer`：预分配缓冲区大小
  - `fmt`：是否格式化（0=紧凑，1=缩进）
- **返回值**：格式化后的字符串

## 四、内存管理函数

### 1. cJSON_Delete
```c
void cJSON_Delete(cJSON *c);
```
- **功能**：删除 cJSON 对象及其所有子节点（递归释放）
- **参数**：要删除的 cJSON 对象
- **注意**：这是释放 cJSON 对象内存的唯一正确方法

### 2. cJSON_InitHooks
```c
void cJSON_InitHooks(cJSON_Hooks* hooks);
```
- **功能**：初始化内存管理钩子
- **参数**：钩子结构体指针，传 NULL 则恢复默认 malloc/free

## 五、查询函数

### 1. cJSON_GetArraySize
```c
int cJSON_GetArraySize(cJSON *array);
```
- **功能**：获取数组（或对象）中元素个数
- **参数**：cJSON 数组/对象节点
- **返回值**：元素个数

### 2. cJSON_GetArrayItem
```c
cJSON *cJSON_GetArrayItem(cJSON *array, int item);
```
- **功能**：获取数组中指定索引的元素
- **参数**：
  - `array`：cJSON 数组节点
  - `item`：索引号（从 0 开始）
- **返回值**：成功返回元素指针，失败返回 NULL

### 3. cJSON_GetObjectItem
```c
cJSON *cJSON_GetObjectItem(cJSON *object, const char *string);
```
- **功能**：根据键名获取对象中的元素（不区分大小写）
- **参数**：
  - `object`：cJSON 对象节点
  - `string`：键名
- **返回值**：成功返回元素指针，失败返回 NULL
- **使用示例**：
  ```c
  cJSON *name_item = cJSON_GetObjectItem(root, "name");
  if (name_item) {
      printf("姓名: %s\n", name_item->valuestring);
  }
  ```

## 六、创建节点函数

### 基础类型创建函数
```c
cJSON *cJSON_CreateNull(void);                    // 创建 null 节点
cJSON *cJSON_CreateTrue(void);                    // 创建 true 节点
cJSON *cJSON_CreateFalse(void);                   // 创建 false 节点
cJSON *cJSON_CreateBool(int b);                   // 创建布尔节点
cJSON *cJSON_CreateNumber(double num);            // 创建数值节点
cJSON *cJSON_CreateString(const char *string);    // 创建字符串节点
cJSON *cJSON_CreateArray(void);                   // 创建空数组节点
cJSON *cJSON_CreateObject(void);                  // 创建空对象节点
```

### 批量创建函数
```c
cJSON *cJSON_CreateIntArray(const int *numbers, int count);       // 从整型数组创建
cJSON *cJSON_CreateFloatArray(const float *numbers, int count);   // 从浮点数组创建
cJSON *cJSON_CreateDoubleArray(const double *numbers, int count); // 从双精度数组创建
cJSON *cJSON_CreateStringArray(const char **strings, int count);  // 从字符串数组创建
```

## 七、操作函数

### 添加元素
```c
void cJSON_AddItemToArray(cJSON *array, cJSON *item);  // 向数组追加元素
void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);  // 向对象添加键值对
void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item); // 添加键值对（字符串为常量）
```

### 添加引用
```c
void cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);  // 向数组添加引用
void cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);  // 向对象添加引用
```

### 删除元素
```c
void cJSON_DeleteItemFromArray(cJSON *array, int which);  // 从数组删除元素
void cJSON_DeleteItemFromObject(cJSON *object, const char *string);  // 从对象删除元素
```

### 插入和替换
```c
void cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);  // 在数组指定位置插入
void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);  // 替换数组元素
void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);  // 替换对象元素
```

## 八、工具函数

### cJSON_Duplicate
```c
cJSON *cJSON_Duplicate(cJSON *item, int recurse);
```
- **功能**：深度复制 cJSON 节点
- **参数**：
  - `item`：要复制的节点
  - `recurse`：是否递归复制子节点
- **返回值**：新的 cJSON 节点

### cJSON_Minify
```c
void cJSON_Minify(char *json);
```
- **功能**：压缩 JSON 字符串（去除空白字符和注释）
- **参数**：可修改的 JSON 字符串

## 九、常用快捷宏

```c
#define cJSON_AddNullToObject(object,name)    cJSON_AddItemToObject(object, name, cJSON_CreateNull())
#define cJSON_AddTrueToObject(object,name)    cJSON_AddItemToObject(object, name, cJSON_CreateTrue())
#define cJSON_AddFalseToObject(object,name)   cJSON_AddItemToObject(object, name, cJSON_CreateFalse())
#define cJSON_AddBoolToObject(object,name,b)  cJSON_AddItemToObject(object, name, cJSON_CreateBool(b))
#define cJSON_AddNumberToObject(object,name,n) cJSON_AddItemToObject(object, name, cJSON_CreateNumber(n))
#define cJSON_AddStringToObject(object,name,s) cJSON_AddItemToObject(object, name, cJSON_CreateString(s))
```

## 十、典型使用示例

### 1. 创建 JSON 对象并添加数据
```c
cJSON *root = cJSON_CreateObject();
cJSON_AddStringToObject(root, "name", "张三");
cJSON_AddNumberToObject(root, "age", 25);
cJSON_AddBoolToObject(root, "married", 1);

char *json_str = cJSON_Print(root);
printf("%s\n", json_str);
free(json_str);
cJSON_Delete(root);
```

### 2. 解析 JSON 并提取数据
```c
const char *json_text = "{\"name\":\"张三\",\"age\":25,\"scores\":[85,92,78]}";
cJSON *root = cJSON_Parse(json_text);

if (root) {
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *age = cJSON_GetObjectItem(root, "age");
    cJSON *scores = cJSON_GetObjectItem(root, "scores");
    
    if (name) printf("姓名: %s\n", name->valuestring);
    if (age) printf("年龄: %d\n", age->valueint);
    
    if (scores && cJSON_GetArraySize(scores) > 0) {
        for (int i = 0; i < cJSON_GetArraySize(scores); i++) {
            cJSON *score = cJSON_GetArrayItem(scores, i);
            printf("成绩%d: %d\n", i+1, score->valueint);
        }
    }
    
    cJSON_Delete(root);
}
```

## 注意事项

1. **内存管理**：所有通过 `cJSON_Parse` 或 `cJSON_Create` 函数创建的对象最终都要通过 `cJSON_Delete` 释放
2. **字符串处理**：使用 `cJSON_Print` 系列函数返回的字符串需要手动调用 `free` 释放
3. **类型检查**：访问节点值之前应先确认其类型，避免类型不匹配导致的问题
4. **错误处理**：解析 JSON 时应检查返回值是否为 NULL，防止空指针访问