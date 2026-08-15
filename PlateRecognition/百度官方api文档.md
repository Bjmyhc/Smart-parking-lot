# 车牌识别 API 文档

## 一、接口描述

支持识别中国大陆机动车蓝牌、黄牌（单双行）、绿牌、大型新能源（黄绿）、领使馆车牌、警牌、武警牌（单双行）、军牌（单双行）、港澳出入境车牌、农用车牌、民航车牌、非机动车车牌（北京地区）的地域编号和车牌号，并能同时识别图像中的多张车牌。

> 视频教程请参见 [车牌识别API调用教程（视频版）]
>
> 在线调试：您可以在 [示例代码中心] 中调试该接口，可进行签名验证、查看在线调用的请求内容和返回结果、示例代码的自动生成。

---

## 二、请求说明

### 1. 请求示例

- **HTTP 方法**：`POST`
- **请求 URL**：`https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate`

### 2. URL 参数

| 参数           | 值                                                                                                  |
| -------------- | --------------------------------------------------------------------------------------------------- |
| `access_token` | 通过 API Key 和 Secret Key 获取的 access_token，参考“Access Token 获取”                              |

### 3. Header

| 参数           | 值                                   |
| -------------- | ------------------------------------ |
| `Content-Type` | `application/x-www-form-urlencoded`  |

> Body 中放置请求参数，参数详情如下：

### 4. 请求参数

| 参数              | 是否必选     | 类型     | 说明                                                                                                                                                                                                                                                                                                                                                  |
| ----------------- | ------------ | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `image`           | 和 url 二选一 | `string` | 图像数据，base64 编码后进行 urlencode，要求 base64 编码和 urlencode 后大小不超过 4M，最短边至少 15px，最长边最大 4096px，支持 jpg/jpeg/png/bmp 格式                                                                                                                                                                                                                              |
| `url`             | 和 image 二选一 | `string` | 图片完整 URL，URL 长度不超过 1024 字节，URL 对应的图片 base64 编码后大小不超过 4M，最短边至少 15px，最长边最大 4096px，支持 jpg/jpeg/png/bmp 格式，当 image 字段存在时 url 字段失效。**请注意关闭 URL 防盗链**                                                                                                                                            |
| `multi_detect`    | 否         | `string` | 是否检测多张车牌，默认为 `false`，当置为 `true` 的时候可以对一张图片内的多张车牌进行识别                                                                                                                                                                                                                                                            |
| `multi_scale`     | 否         | `string` | 在高拍等车牌较小的场景下可开启，默认为 `false`，当置为 `true` 时，能够提高对较小车牌的检测和识别。**提示**：当前新版车牌识别能力无需开启此参数，即可支持在高拍场景下的准确识别，此参数已无效，即将下线                                                                                                                                                    |
| `detect_complete` | 否         | `string` | 是否开启车牌遮挡检测功能，默认为 `false`，不开启；<br>- `true`：开启遮挡检测<br>- `false`：不开启遮挡检测                                                                                                                                                                                                                                            |
| `detect_risk`     | 否         | `string` | 是否开启车牌 PS 检测功能，默认为 `false`，不开启；<br>- `true`：开启 PS 检测<br>- `false`：不开启 PS 检测                                                                                                                                                                                                                                              |

---

## 三、请求代码示例

> **提示一**：使用示例代码前，请记得替换其中的示例 Token、图片地址或 Base64 信息。
>
> **提示二**：部分语言依赖的类或库，请在代码注释中查看下载地址。

### Bash

```bash
curl -i -k 'https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate?access_token=【调用鉴权接口获取的token】' \
  --data 'image=【图片Base64编码，需UrlEncode】' \
  -H 'Content-Type:application/x-www-form-urlencoded'
```

### Python

```python
# encoding:utf-8

import requests
import base64

'''
车牌识别
'''

request_url = "https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate"

# 二进制方式打开图片文件
f = open('[本地文件]', 'rb')
img = base64.b64encode(f.read())

params = {"image": img}
access_token = '[调用鉴权接口获取的token]'
request_url = request_url + "?access_token=" + access_token
headers = {'content-type': 'application/x-www-form-urlencoded'}
response = requests.post(request_url, data=params, headers=headers)
if response:
    print(response.json())
```

### Java

```java
package com.baidu.ai.aip;

import com.baidu.ai.aip.utils.Base64Util;
import com.baidu.ai.aip.utils.FileUtil;
import com.baidu.ai.aip.utils.HttpUtil;

import java.net.URLEncoder;

/**
 * 车牌识别
 */
public class LicensePlate {

    /**
     * 重要提示代码中所需工具类
     * FileUtil, Base64Util, HttpUtil, GsonUtils 请从
     * https://ai.baidu.com/file/658A35ABAB2D404FBF903F64D47C1F72
     * https://ai.baidu.com/file/C8D81F3301E24D2892968F09AE1AD6E2
     * https://ai.baidu.com/file/544D677F5D4E4F17B4122FBD60DB82B3
     * https://ai.baidu.com/file/470B3ACCA3FE43788B5A963BF0B625F3
     * 下载
     */
    public static String licensePlate() {
        // 请求 url
        String url = "https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate";
        try {
            // 本地文件路径
            String filePath = "[本地文件路径]";
            byte[] imgData = FileUtil.readFileByBytes(filePath);
            String imgStr = Base64Util.encode(imgData);
            String imgParam = URLEncoder.encode(imgStr, "UTF-8");

            String param = "image=" + imgParam;

            // 注意这里仅为了简化编码每一次请求都去获取 access_token，
            // 线上环境 access_token 有过期时间，客户端可自行缓存，过期后重新获取。
            String accessToken = "[调用鉴权接口获取的token]";

            String result = HttpUtil.post(url, accessToken, param);
            System.out.println(result);
            return result;
        } catch (Exception e) {
            e.printStackTrace();
        }
        return null;
    }

    public static void main(String[] args) {
        LicensePlate.licensePlate();
    }
}
```

### C++

```cpp
#include <iostream>
#include <curl/curl.h>

// libcurl 库下载链接：https://curl.haxx.se/download.html
// jsoncpp 库下载链接：https://github.com/open-source-parsers/jsoncpp/

const static std::string request_url = "https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate";
static std::string licensePlate_result;

/**
 * curl 发送 http 请求调用的回调函数，
 * 回调函数中对返回的 json 格式的 body 进行了解析，
 * 解析结果储存在全局的静态变量当中
 *
 * @param 参数定义见 libcurl 文档
 * @return 返回值定义见 libcurl 文档
 */
static size_t callback(void *ptr, size_t size, size_t nmemb, void *stream) {
    // 获取到的 body 存放在 ptr 中，先将其转换为 string 格式
    licensePlate_result = std::string((char *)ptr, size * nmemb);
    return size * nmemb;
}

/**
 * 车牌识别
 *
 * @return 调用成功返回 0，发生错误返回其他错误码
 */
int licensePlate(std::string &json_result, const std::string &access_token) {
    std::string url = request_url + "?access_token=" + access_token;
    CURL *curl = NULL;
    CURLcode result_code;
    int is_success;
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.data());
        curl_easy_setopt(curl, CURLOPT_POST, 1);
        curl_httppost *post = NULL;
        curl_httppost *last = NULL;
        curl_formadd(&post, &last,
                     CURLFORM_COPYNAME, "image",
                     CURLFORM_COPYCONTENTS, "【base64_img】",
                     CURLFORM_END);

        curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);
        result_code = curl_easy_perform(curl);
        if (result_code != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(result_code));
            is_success = 1;
            return is_success;
        }
        json_result = licensePlate_result;
        curl_easy_cleanup(curl);
        is_success = 0;
    } else {
        fprintf(stderr, "curl_easy_init() failed.");
        is_success = 1;
    }
    return is_success;
}
```

### PHP

```php
<?php
/**
 * 发起 http post 请求 (REST API)，并获取 REST 请求的结果
 *
 * @param string $url
 * @param string $param
 * @return - http response body if succeeds, else false.
 */
function request_post($url = '', $param = '')
{
    if (empty($url) || empty($param)) {
        return false;
    }

    $postUrl = $url;
    $curlPost = $param;
    // 初始化 curl
    $curl = curl_init();
    curl_setopt($curl, CURLOPT_URL, $postUrl);
    curl_setopt($curl, CURLOPT_HEADER, 0);
    // 要求结果为字符串且输出到屏幕上
    curl_setopt($curl, CURLOPT_RETURNTRANSFER, 1);
    curl_setopt($curl, CURLOPT_SSL_VERIFYPEER, false);
    // post 提交方式
    curl_setopt($curl, CURLOPT_POST, 1);
    curl_setopt($curl, CURLOPT_POSTFIELDS, $curlPost);
    // 运行 curl
    $data = curl_exec($curl);
    curl_close($curl);

    return $data;
}

$token = '[调用鉴权接口获取的token]';
$url = 'https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate?access_token=' . $token;
$img = file_get_contents('[本地文件路径]');
$img = base64_encode($img);
$bodys = array(
    'image' => $img
);
$res = request_post($url, $bodys);

var_dump($res);
```

### C&#35;

```csharp
using System;
using System.IO;
using System.Net;
using System.Text;
using System.Web;

namespace com.baidu.ai
{
    public class LicensePlate
    {
        // 车牌识别
        public static string licensePlate()
        {
            string token = "[调用鉴权接口获取的token]";
            string host = "https://aip.baidubce.com/rest/2.0/ocr/v1/license_plate?access_token=" + token;
            Encoding encoding = Encoding.Default;
            HttpWebRequest request = (HttpWebRequest)WebRequest.Create(host);
            request.Method = "post";
            request.KeepAlive = true;
            // 图片的 base64 编码
            string base64 = getFileBase64("[本地图片文件]");
            String str = "image=" + HttpUtility.UrlEncode(base64);
            byte[] buffer = encoding.GetBytes(str);
            request.ContentLength = buffer.Length;
            request.GetRequestStream().Write(buffer, 0, buffer.Length);
            HttpWebResponse response = (HttpWebResponse)request.GetResponse();
            StreamReader reader = new StreamReader(response.GetResponseStream(), Encoding.Default);
            string result = reader.ReadToEnd();
            Console.WriteLine("车牌识别:");
            Console.WriteLine(result);
            return result;
        }

        public static String getFileBase64(String fileName) {
            FileStream filestream = new FileStream(fileName, FileMode.Open);
            byte[] arr = new byte[filestream.Length];
            filestream.Read(arr, 0, (int)filestream.Length);
            string baser64 = Convert.ToBase64String(arr);
            filestream.Close();
            return baser64;
        }
    }
}
```

---

## 四、返回说明

### 1. 返回参数

| 参数                | 是否必须 | 类型       | 说明                                                                                                                                                                                                                  |
| ------------------- | -------- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `log_id`            | 是       | `uint64`   | 唯一的 log id，用于问题定位                                                                                                                                                                                          |
| `words_result`      | 是       | `array[]`  | 识别结果数组                                                                                                                                                                                                        |
| ? `color`          | 是       | `string`   | 车牌颜色：支持 `blue`、`green`、`yellow`、`white`、`black`、`yellow_green`（新能源大型汽车黄绿车牌）、`unknow`（未知颜色）、`penyin`（大货车喷印车牌）                                                                      |
| ? `number`         | 是       | `string`   | 车牌号码                                                                                                                                                                                                              |
| ? `probability`    | 是       | `string`   | 7 个数字分别为车牌中每个字符的置信度（从左往右），区间为 0-1，如需平均置信度，将全部数值相加，计算平均值即可                                                                                                                |
| ? `vertexes_location` | 是   | `array[]`  | 返回文字外接多边形顶点位置                                                                                                                                                                                            |
|   ? `x`            | 是       | `uint32`   | 水平坐标（坐标 0 点为左上角）                                                                                                                                                                                        |
|   ? `y`            | 是       | `uint32`   | 垂直坐标（坐标 0 点为左上角）                                                                                                                                                                                        |
| ? `cover_info`     | 否       | `string`   | 判断车牌有没有被遮挡，当 `detect_complete=true` 时生效；<br>- `incomplete`：没有被遮挡<br>- `complete`：被遮挡                                                                                                          |
| ? `edit_tool`      | 否       | `string`   | 判断车牌有没有被遮挡，当 `detect_risk=true` 时生效；如果检测车牌被编辑过，该字段指定编辑软件名称，如：`Adobe Photoshop CC 2014 (Macintosh)`，如果没有被编辑过则返回值为空                                                       |

### 2. 返回示例

```json
{
    "words_result": [
        {
            "color": "blue",
            "number": "京KBT355",
            "probability": [
                0.9999992847,
                0.999999404,
                0.9999910593,
                0.9999765158,
                0.999994874,
                0.9998959303,
                0.9999984503
            ],
            "vertexes_location": [
                {
                    "x": 495,
                    "y": 589
                },
                {
                    "x": 800,
                    "y": 587
                },
                {
                    "x": 800,
                    "y": 676
                },
                {
                    "x": 496,
                    "y": 678
                }
            ]
        }
    ],
    "log_id": "6845817085824549137"
}
```
