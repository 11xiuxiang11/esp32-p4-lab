/*
 * mjpeg_server.c —— P5-3：Wi-Fi 图传（第一步：HTTP 单帧 /jpg）
 *
 * 编译验证：idf.bat build
 * 配置    ：EXP_MJPEG=18；SC2336 1024x600 RGB565；JPEG 质量 70、4:2:2
 * 结果    ：0 error 0 warning（2026-11-22）
 * 日期    ：2026-11-22
 *
 * 零拷贝链路：
 *   /dev/video0 的 mmap 缓冲（RGB565 1.2MB）
 *     → jpeg_encoder_process（DMA2D 从 PSRAM 读，硬件 JPEG 编码）
 *     → 结果投递到一个"槽位" → httpd 任务取出 → httpd_resp_send
 *     → lwIP → esp-hosted → SDIO → C6 → 浏览器
 *
 * 为什么编码不放 HTTP 回调里：
 *   回调跑在 httpd 任务里，发一张图要几百 ms（受 Wi-Fi 带宽限制）。如果回调
 *   自己去 VIDIOC_DQBUF，慢客户端就会把相机流卡死（没有空闲缓冲可写）。
 *   所以相机任务独占 fd：DQBUF → 有需求就编码 → QBUF，httpd 只"取一张发一张"。
 *
 * 为什么用槽位 + 队列而不是一把锁：
 *   编码结果的所有权要转交给 httpd 任务，发送期间相机任务不能碰这块内存。
 *   队列传"第几号槽位"，用完再还回空闲队列——正好是 B3 队列那套语义。
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

#include "wifi_sta.h"
#include "mjpeg_server.h"

static const char *TAG = "MJPEG";

/* ---- 相机（SCCB 走 I2C1，与触摸共用一条总线）---- */
#define CAM_I2C_PORT            I2C_NUM_1
#define CAM_I2C_SCL             GPIO_NUM_8
#define CAM_I2C_SDA             GPIO_NUM_7
#define SCCB_FREQ_HZ            100000

#define FRAME_W                 1024
#define FRAME_H                 600
#define FRAME_BYTES             (FRAME_W * FRAME_H * 2)   /* RGB565：每像素 2 字节 */
#define CAM_BUF_COUNT           2
#define CAM_TASK_STACK_BYTES    8192
#define CAM_TASK_PRIO           6
#define CAM_TASK_CORE           1
#define CAPTURE_LOG_EVERY       30                        /* 每 30 帧打一次采集日志 */

/* ---- JPEG 编码 ---- */
#define JPG_SLOT_COUNT          3
#define JPG_SLOT_BYTES          (256 * 1024)   /* 单张 JPEG 缓冲：1024x600 质量 70 约 60~120KB */
#define JPG_READY_DEPTH         2              /* 待发送队列深度：2 帧够大了，超了就该丢 */
#define JPEG_QUALITY            70
#define JPEG_TIMEOUT_MS         200            /* 单帧编码超时：正常 <50ms，超了说明硬件/内存有问题 */

/* ---- HTTP ---- */
#define HTTPD_PORT              80
#define HTTPD_STACK_BYTES       6144
#define SNAPSHOT_TIMEOUT_MS     2000           /* 一次 /jpg 最多等多久拿到新帧 */
#define STREAM_BOUNDARY         "p4frame"      /* multipart 分隔符，随便起名，别和 JPEG 内容撞 */
#define STREAM_LOG_EVERY        30             /* 流每发 30 帧打一次统计 */
#define WIFI_IP_TIMEOUT_MS      15000
#define WIFI_RETRY_DELAY_MS     3000

/* 一个编码槽位：data 由 jpeg_alloc_encoder_mem 分配（cache line 对齐，DMA 才能写） */
typedef struct {
    uint8_t *data;
    size_t   capacity;
} jpg_slot_t;

/* 相机任务 → httpd 任务的一封"邮件"：哪号槽位、多长、编码花了多久 */
typedef struct {
    uint8_t  slot;
    size_t   len;
    uint32_t encode_us;
    uint32_t seq;
} jpg_msg_t;

static jpg_slot_t         s_slots[JPG_SLOT_COUNT];
static QueueHandle_t      s_free_slots;   /* 空闲槽位号（uint8_t）：生产者取，消费者还 */
static QueueHandle_t      s_ready_jpegs;  /* 编好的帧（jpg_msg_t） */
static SemaphoreHandle_t  s_demand;       /* 计数信号量：有几个"我要一张新图"的请求在排队 */
static jpeg_encoder_handle_t s_encoder;
static jpeg_encode_cfg_t  s_encode_cfg = {
    .height = FRAME_H,
    .width = FRAME_W,
    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
    .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
    .image_quality = JPEG_QUALITY,
};

static int      s_cam_fd = -1;
static uint8_t *s_cam_buf[CAM_BUF_COUNT];
static uint32_t s_jpg_seq;   /* 只在相机任务里自增，不用加锁 */
static uint32_t s_last_jpg_bytes;   /* 最近一张 JPEG 的字节数，给周期日志用 */
static uint32_t s_last_encode_us;   /* 最近一张的编码耗时，给周期日志用 */

/*============================================================================
 * JPEG 编码引擎 + 槽位池
 *============================================================================*/

/* 初始化编码器硬件，并分配 JPG_SLOT_COUNT 个输出缓冲。
 * 返回 ESP_OK 才能继续；失败说明 PSRAM 不够或 DMA2D 被占死。 */
static esp_err_t jpeg_pool_init(void)
{
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,          /* 0 = 驱动自己挑（1/2/3） */
        .timeout_ms = JPEG_TIMEOUT_MS,
    };
    esp_err_t err = jpeg_new_encoder_engine(&engine_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine: %s", esp_err_to_name(err));
        return err;
    }

    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    for (int i = 0; i < JPG_SLOT_COUNT; i++) {
        size_t allocated = 0;
        s_slots[i].data = (uint8_t *)jpeg_alloc_encoder_mem(JPG_SLOT_BYTES, &mem_cfg, &allocated);
        if (s_slots[i].data == NULL) {
            ESP_LOGE(TAG, "第 %d 号 JPEG 缓冲分配失败（%d 字节）", i, JPG_SLOT_BYTES);
            return ESP_ERR_NO_MEM;
        }
        s_slots[i].capacity = allocated;
    }

    s_free_slots = xQueueCreate(JPG_SLOT_COUNT, sizeof(uint8_t));
    s_ready_jpegs = xQueueCreate(JPG_READY_DEPTH, sizeof(jpg_msg_t));
    s_demand = xSemaphoreCreateCounting(4, 0);
    if (s_free_slots == NULL || s_ready_jpegs == NULL || s_demand == NULL) {
        ESP_LOGE(TAG, "创建队列/信号量失败");
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < JPG_SLOT_COUNT; i++) {
        (void)xQueueSend(s_free_slots, &i, 0);
    }
    return ESP_OK;
}

/* 把一帧 RGB565 编码成 JPEG 并投递给 httpd 任务。
 * rgb 是相机 mmap 缓冲（此时缓冲还归我们，驱动不会写它），本函数只读。
 * 返回 ESP_OK = 已投递；ESP_ERR_NO_MEM = 没有空槽位（消费端没跟上），这一帧丢弃。 */
static esp_err_t encode_and_post(const uint8_t *rgb)
{
    uint8_t slot = 0;
    if (xQueueReceive(s_free_slots, &slot, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t out_size = 0;
    int64_t t_start_us = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s_encoder, &s_encode_cfg, rgb, FRAME_BYTES,
                                         s_slots[slot].data, (uint32_t)s_slots[slot].capacity,
                                         &out_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "编码失败: %s（输出缓冲 %u 字节可能不够）",
                 esp_err_to_name(err), (unsigned)s_slots[slot].capacity);
        (void)xQueueSend(s_free_slots, &slot, portMAX_DELAY);
        return err;
    }

    jpg_msg_t msg = {
        .slot = slot,
        .len = out_size,
        .encode_us = (uint32_t)(esp_timer_get_time() - t_start_us),
        .seq = ++s_jpg_seq,
    };
    s_last_jpg_bytes = out_size;
    s_last_encode_us = msg.encode_us;
    if (xQueueSend(s_ready_jpegs, &msg, 0) != pdTRUE) {
        (void)xQueueSend(s_free_slots, &slot, portMAX_DELAY);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*============================================================================
 * 相机：打开 /dev/video0、设格式、把缓冲全部挂进驱动队列
 *============================================================================*/
static esp_err_t camera_open(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = CAM_I2C_PORT,
        .sda_io_num = CAM_I2C_SDA,
        .scl_io_num = CAM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err = i2c_new_master_bus(&i2c_cfg, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus,
            .freq = SCCB_FREQ_HZ,
        },
        .reset_pin = GPIO_NUM_NC,
        .pwdn_pin = GPIO_NUM_NC,
        .dont_init_ldo = false,
    };
    esp_video_init_config_t video_cfg = { .csi = &csi_cfg };
    err = esp_video_init(&video_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init: %s（相机排线？）", esp_err_to_name(err));
        return err;
    }

    s_cam_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_cam_fd < 0) {
        ESP_LOGE(TAG, "open %s 失败: errno=%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno);
        return ESP_FAIL;
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_cam_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT 失败");
        return ESP_FAIL;
    }
    fmt.fmt.pix.width = FRAME_W;
    fmt.fmt.pix.height = FRAME_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(s_cam_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT(%dx%d RGB565) 失败", FRAME_W, FRAME_H);
        return ESP_FAIL;
    }
    if (fmt.fmt.pix.bytesperline != FRAME_W * 2) {
        /* 编码器按"紧凑排布"读输入；stride 不等就说明每行有填充，图会斜 */
        ESP_LOGW(TAG, "stride=%u != %u，编码输入不是紧凑排布，画面可能错位",
                 (unsigned)fmt.fmt.pix.bytesperline, (unsigned)(FRAME_W * 2));
    }

    struct v4l2_requestbuffers req = {
        .count = CAM_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_cam_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS 失败");
        return ESP_FAIL;
    }

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (ioctl(s_cam_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%d) 失败", i);
            return ESP_FAIL;
        }
        s_cam_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, s_cam_fd, buf.m.offset);
        if (s_cam_buf[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap(%d) 失败", i);
            s_cam_buf[i] = NULL;
            return ESP_FAIL;
        }
        if (ioctl(s_cam_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF(%d) 失败", i);
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON 失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "相机已启动：%dx%d RGB565", FRAME_W, FRAME_H);
    return ESP_OK;
}

/* 相机任务：DQBUF → 有人要图就编码 → QBUF。
 * 编码期间驱动没有空闲缓冲，会丢帧——对"图传"来说这正是想要的：不积压。 */
static void camera_task(void *arg)
{
    (void)arg;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint32_t frames = 0;
    uint32_t encoded = 0;
    int64_t t_log_us = esp_timer_get_time();

    for (;;) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_cam_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF 失败: errno=%d", errno);
            break;
        }
        frames++;

        /* 没人看图就不编码：空转时相机任务几乎不占 CPU */
        if (xSemaphoreTake(s_demand, 0) == pdTRUE) {
            if (encode_and_post(s_cam_buf[buf.index]) == ESP_OK) {
                encoded++;
            }
        }

        if (ioctl(s_cam_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF 失败: errno=%d", errno);
            break;
        }

        if ((frames % CAPTURE_LOG_EVERY) == 0) {
            int64_t now_us = esp_timer_get_time();
            if (encoded > 0) {
                ESP_LOGI(TAG, "采集 %.1f fps，累计编码 %u 帧，最近一张 %.1f KB / %u ms",
                         (float)CAPTURE_LOG_EVERY * 1000000.0f / (float)(now_us - t_log_us),
                         (unsigned)encoded,
                         (float)s_last_jpg_bytes / 1024.0f,
                         (unsigned)(s_last_encode_us / 1000));
            } else {
                ESP_LOGI(TAG, "采集 %.1f fps，累计编码 0 帧（还没人要看图）",
                         (float)CAPTURE_LOG_EVERY * 1000000.0f / (float)(now_us - t_log_us));
            }
            t_log_us = now_us;
        }
    }

    ioctl(s_cam_fd, VIDIOC_STREAMOFF, &type);
    close(s_cam_fd);
    s_cam_fd = -1;
    vTaskDelete(NULL);
}

/*============================================================================
 * HTTP 处理函数
 *============================================================================*/

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>ESP32-P4 图传</title></head><body style=\"font-family:sans-serif\">"
        "<h2>ESP32-P4 / SC2336 图传</h2>"
        "<p>连续流：<a href=\"/stream\">/stream</a>（MJPEG，浏览器直接播）</p>"
        "<p>单帧：<a href=\"/jpg\">/jpg</a>（每刷新一次取一张新图）</p>"
        "<img src=\"/stream\" width=\"1024\" alt=\"stream\">"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/* GET /jpg：下单要一张新帧 → 等编码完成 → 原样发回去。
 * 不返回"缓存里的旧图"：先 xSemaphoreGive 再收队列，收到的一定是这一单之后编的。 */
static esp_err_t jpg_get_handler(httpd_req_t *req)
{
    if (xSemaphoreGive(s_demand) != pdTRUE) {
        /* 计数信号量满（已经排了 4 个请求）说明有客户端在狂刷，直接拒 */
        ESP_LOGW(TAG, "/jpg 请求太多，先拒掉这一个");
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_send(req, "请求太密", HTTPD_RESP_USE_STRLEN);
    }

    jpg_msg_t msg = {0};
    if (xQueueReceive(s_ready_jpegs, &msg, pdMS_TO_TICKS(SNAPSHOT_TIMEOUT_MS)) != pdTRUE) {
        /* 下单后没人来还（相机挂了/编码一直失败）：不还需求会一直挂着，清一个 */
        (void)xSemaphoreTake(s_demand, 0);
        ESP_LOGW(TAG, "/jpg 等帧超时（%d ms）", SNAPSHOT_TIMEOUT_MS);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "没有拿到新帧", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int64_t t_send_us = esp_timer_get_time();
    esp_err_t err = httpd_resp_send(req, (const char *)s_slots[msg.slot].data, (ssize_t)msg.len);
    uint32_t send_us = (uint32_t)(esp_timer_get_time() - t_send_us);

    /* 槽位用完必须还，否则三次请求之后就没有空槽了 */
    (void)xQueueSend(s_free_slots, &msg.slot, portMAX_DELAY);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "/jpg 发送中断: %s（客户端提前关了？）", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "/jpg #%u：%u 字节 | 编码 %u ms | 发送 %u ms",
             (unsigned)msg.seq, (unsigned)msg.len,
             (unsigned)(msg.encode_us / 1000), (unsigned)(send_us / 1000));
    return ESP_OK;
}

/* GET /stream：一条连接持续下发 MJPEG（multipart/x-mixed-replace）。
 * 每一帧的格式：
 *   --p4frame\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n<JPEG 原始字节>\r\n
 * 有 Content-Length，浏览器自己就能切帧，不依赖扫描分隔符。
 * 退出条件只有一个：发送失败（客户端关页面/断网）。 */
static esp_err_t stream_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    uint32_t sent_frames = 0;
    uint32_t window_frames = 0;
    uint64_t window_bytes = 0;
    int64_t t_window_us = esp_timer_get_time();

    for (;;) {
        /* 下单：相机任务看到需求才编码，没人看就不烧 CPU */
        (void)xSemaphoreGive(s_demand);

        jpg_msg_t msg = {0};
        if (xQueueReceive(s_ready_jpegs, &msg, pdMS_TO_TICKS(SNAPSHOT_TIMEOUT_MS)) != pdTRUE) {
            (void)xSemaphoreTake(s_demand, 0);   /* 收回这单需求，别让它越积越多 */
            ESP_LOGW(TAG, "/stream 等帧超时，结束本次流（已发 %u 帧）", (unsigned)sent_frames);
            break;
        }

        char header[96];
        int header_len = snprintf(header, sizeof(header),
                                  "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                  STREAM_BOUNDARY, (unsigned)msg.len);

        esp_err_t err = httpd_resp_send_chunk(req, header, header_len);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)s_slots[msg.slot].data, (ssize_t)msg.len);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        /* 槽位用完立刻还，否则 3 帧之后就没缓冲可编码了 */
        (void)xQueueSend(s_free_slots, &msg.slot, portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGI(TAG, "/stream 客户端断开（%s），共发 %u 帧",
                     esp_err_to_name(err), (unsigned)sent_frames);
            break;
        }

        sent_frames++;
        window_frames++;
        window_bytes += msg.len;
        if ((sent_frames % STREAM_LOG_EVERY) == 0) {
            int64_t now_us = esp_timer_get_time();
            ESP_LOGI(TAG, "/stream 已发 %u 帧，%.1f fps，平均 %.1f KB/帧",
                     (unsigned)sent_frames,
                     (float)window_frames * 1000000.0f / (float)(now_us - t_window_us),
                     (float)window_bytes / (float)window_frames / 1024.0f);
            window_frames = 0;
            window_bytes = 0;
            t_window_us = now_us;
        }
    }

    /* 正常收尾：0 长度块表示流结束（客户端已断开时这步会失败，故意忽略） */
    (void)httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t s_uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t s_uri_jpg = {
    .uri = "/jpg",
    .method = HTTP_GET,
    .handler = jpg_get_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t s_uri_stream = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_get_handler,
    .user_ctx = NULL,
};

static esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTPD_PORT;
    cfg.stack_size = HTTPD_STACK_BYTES;
    /* lwIP 总共才 10 个 socket；给 httpd 留 4 个，其余给心跳/调试 */
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t e_root = httpd_register_uri_handler(server, &s_uri_root);
    esp_err_t e_jpg = httpd_register_uri_handler(server, &s_uri_jpg);
    esp_err_t e_stream = httpd_register_uri_handler(server, &s_uri_stream);
    if (e_root != ESP_OK || e_jpg != ESP_OK || e_stream != ESP_OK) {
        ESP_LOGE(TAG, "注册 URI 失败: /=%s /jpg=%s /stream=%s",
                 esp_err_to_name(e_root), esp_err_to_name(e_jpg), esp_err_to_name(e_stream));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*============================================================================
 * 启动流程：相机 → JPEG 自检 → Wi-Fi → HTTP 服务
 *============================================================================*/

/* 开机自检：不经过 Wi-Fi，让相机编一张，把大小和耗时写进日志。
 * 目的：AP 不在/没网时也能证明"相机 → 硬件 JPEG"这段是通的，把故障面缩小。 */
static void mjpeg_selftest(void)
{
    (void)xSemaphoreGive(s_demand);

    jpg_msg_t msg = {0};
    if (xQueueReceive(s_ready_jpegs, &msg, pdMS_TO_TICKS(SNAPSHOT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "自检：%d ms 内没拿到帧（相机没出图？）", SNAPSHOT_TIMEOUT_MS);
        return;
    }
    ESP_LOGI(TAG, "自检通过：JPEG %u 字节 / %dx%d，编码 %u ms",
             (unsigned)msg.len, FRAME_W, FRAME_H, (unsigned)(msg.encode_us / 1000));
    (void)xQueueSend(s_free_slots, &msg.slot, portMAX_DELAY);
}

static void boot_task(void *arg)
{
    (void)arg;

    if (jpeg_pool_init() != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 编码器初始化失败");
        vTaskDelete(NULL);
        return;
    }

    if (camera_open() != ESP_OK) {
        ESP_LOGE(TAG, "相机初始化失败");
        vTaskDelete(NULL);
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(camera_task, "cam_cap", CAM_TASK_STACK_BYTES,
                                            NULL, CAM_TASK_PRIO, NULL, CAM_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建相机任务失败");
        vTaskDelete(NULL);
        return;
    }

    /* 先证硬件，再接网络：没网也能看到"编码成功"这一行 */
    mjpeg_selftest();

    if (!wifi_sta_start()) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败");
        vTaskDelete(NULL);
        return;
    }

    if (http_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HTTP 服务已启动（等 Wi-Fi 拿到 IP 就能访问）");

    while (!wifi_sta_wait_ip(WIFI_IP_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "等 IP 超时，%d ms 后重拨（AP 不在范围时 reason=201）",
                 WIFI_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
        wifi_sta_kick();
    }

    char ip_str[16] = "0.0.0.0";
    if (!wifi_sta_get_ip_str(ip_str, sizeof(ip_str))) {
        ESP_LOGW(TAG, "拿不到 IP 字符串，用 0.0.0.0 占位");
    }
    ESP_LOGI(TAG, "板子地址：http://%s/jpg", ip_str);

    vTaskDelete(NULL);
}

void mjpeg_server_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(boot_task, "mjpeg_boot", 8192, NULL,
                                            5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建启动任务失败");
    }
}
