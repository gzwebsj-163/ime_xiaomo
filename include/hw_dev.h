#ifndef HW_DEV_H
#define HW_DEV_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 硬件设备命令编码 */
typedef enum {
    HW_DEV_OPTIONS  = 0x00,
    HW_DEV_CONFIG   = 0x04,
    HW_DEV_PROJECT  = 0x05,
    HW_DEV_DEBUG    = 0x06,
    HW_DEV_DUMP     = 0x07,
    HW_DEV_API      = 0x08,
} hw_dev_cmd_t;

/**
 * 命令条目：命令名字符串、掩码、处理回调
 */
typedef struct {
    const char*         cmd_name;
    uint32_t            mask;
    uint16_t            (*handler)(void *arg);
} hw_dev_address_t;

/**
 * 命令集合容器
 */
typedef struct DEV_POP {
    const hw_dev_address_t* table;
    uint32_t                count;
} DEV_POP_t;

/* 通用设备回调原型 */
typedef uint16_t (*hw_dev_cb_t)(void *arg);

/* ---- 请求上下文 (2026-09-07): dispatch 组包传给 handler ----
 * 命令行协议: "命令名\n\r 参数..." → handler 收到的 arg = hw_dev_req_t*
 *   cmd    = 完整命令串 (含前缀)
 *   params = 前缀之后的参数部分 (跳过 "命令名\n\r ")
 *   ctx    = dispatch 第二参透传 (VM 侧 = KillsVM*)
 * 兼容: handler 不读 arg 时行为与旧版完全一致 (arg 透明可忽略)。 */
typedef struct hw_dev_req {
    const char* cmd;
    const char* params;
    void*       ctx;
} hw_dev_req_t;

/* handler 返回码 (统一语义, 0xFF 保留给 dispatch "未命中") */
#define HWDEV_R_OK           0x00  /* 命中执行成功 */
#define HWDEV_R_NOARGS       0x01  /* 参数缺失 */
#define HWDEV_R_BADARG       0x02  /* 参数非法 (含未知子命令) */
#define HWDEV_R_IOERR        0x03  /* 底层硬件操作失败 */
#define HWDEV_R_NOHANDLE     0x04  /* i2c 句柄无效 */
#define HWDEV_R_UNSUPPORTED  0xE0  /* 平台不支持该外设 */

/**
 * @brief 取最近一次 handler 产出的文本结果 (echo 回显/i2c 读取数据/pwm 状态等)
 * @param buf 输出缓冲; @param cap 缓冲大小; 无结果时填空串
 */
void hw_dev_result(char *buf, uint32_t cap);

/**
 * @brief 设备控制接口
 * @param cmd  hw_dev_cmd_t 命令码
 * @param cb   回调函数指针，可以传NULL
 * @param arg  传给回调的上下文参数
 * @return uint16 状态值
 * 逻辑：cb非NULL调用回调返回回调结果；cb为NULL返回默认常量0x01
 */
uint16_t hw_dev_ctrl(hw_dev_cmd_t cmd, hw_dev_cb_t cb, void *arg);

/**
 * @brief 根据命令字符串查找并执行对应命令
 * @param cmd_str 输入命令字符串
 * @param arg     上下文参数
 * @return 命令返回值；找不到返回0xFF
 */
uint16_t hw_dev_dispatch(const char *cmd_str, void *arg);
uint16_t hw_dev_main(void *arg);
void hw_dev_init(void *arg);
void hw_dev_hook(void *arg);

/**
 * @brief 注册动态命令进 dev_bin (运行时可扩展命令表)
 * @param cmd_name 命令前缀串 (含 "\n\r " 尾缀则按原样匹配)
 * @param mask     命令掩码 (自定义用途)
 * @param cb       回调, 可传 NULL (命中时按 0x00 处理)
 * @return 0 成功; -1 参数非法; -2 撞名; -3 内存不足
 */
int hw_dev_register(const char *cmd_name, uint32_t mask, hw_dev_cb_t cb);

/**
 * @brief 当前动态命令表条数
 */
uint32_t hw_dev_registered(void);

#ifdef __cplusplus
}
#endif

/* C++独有类型，放在extern"C"外面，C编译器不会解析这部分 */
#ifdef __cplusplus
#include <functional>
using hw_dev_handler = std::function<void(uint16_t cmd,uint16_t echo,uint16_t tx,uint16_t rx,uint16_t send)>;

#endif
#endif
