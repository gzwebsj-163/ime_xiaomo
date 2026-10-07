#include "gpio_pwm.h"
#include <stdlib.h>
#include <stdint.h>
typedef struct PWM_VM{
	uint32_t magic;
	channel_config_t ch_cfg;
	channel_time_t ch_time;
	bool inited;
};
#ifdef __cplusplus
extern "C"{
#endif
__attribute__((weak)) PWM_VM*pwm_init(const char* cfg){(void)cfg;PWM_VM *pw = calloc(1,sizeof(PWM_VM));if(!pw)return NULL;}
__attribute__((weak)) PWM_VM*pwm_exec(PWM_VM* hanlder,const char* cmd,const char* expect,int time_out){}
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C"{
#endif
__attribute__((weak))
#ifdef __cplusplus
}
