#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    INPUT_KEY_NONE = 0,
    INPUT_KEY_UP,
    INPUT_KEY_DOWN,
    INPUT_KEY_LEFT,
    INPUT_KEY_RIGHT,
    INPUT_KEY_OK,
} input_key_t;

typedef enum {
    INPUT_PRESS_SHORT = 0,
    INPUT_PRESS_LONG,
} input_press_t;

typedef struct {
    input_key_t   key;
    input_press_t press;
} input_event_t;

typedef uint8_t roast_profile_id_t;

typedef enum {
    ROAST_STATE_IDLE = 0,
    ROAST_STATE_PREHEAT,
    ROAST_STATE_CHARGE,
    ROAST_STATE_ROASTING,
    ROAST_STATE_MANUAL,
    ROAST_STATE_COOLING,
    ROAST_STATE_DONE,
    ROAST_STATE_FAULT,
} roast_state_t;

typedef enum {
    ROAST_FAULT_NONE = 0,
    ROAST_FAULT_SENSOR,
    ROAST_FAULT_OVERTEMP,
    ROAST_FAULT_FAN,
    ROAST_FAULT_DRUM,
} roast_fault_t;

typedef enum {
    FAN_STATE_OFF = 0,
    FAN_STATE_STARTING,
    FAN_STATE_RUNNING,
} fan_state_t;

typedef enum {
    ROAST_CMD_NONE = 0,
    ROAST_CMD_START,
    ROAST_CMD_CONFIRM_CHARGE,
    ROAST_CMD_STOP,
    ROAST_CMD_SET_HEAT,
    ROAST_CMD_SET_FAN,
    ROAST_CMD_MARK_FIRST_CRACK,
    ROAST_CMD_SET_DRUM,
} roast_cmd_type_t;

typedef struct {
    roast_cmd_type_t   type;
    roast_profile_id_t profile;
    uint16_t           batch_g;
    uint8_t            value;
} roast_cmd_t;

typedef struct {
    uint32_t t_ms;
    float    temp_c;
    bool     sensor_ok;
    uint16_t prox_raw;
    bool     prox_alert;
} sensor_sample_t;

typedef struct {
    roast_state_t      state;
    roast_fault_t      fault;
    roast_profile_id_t profile;
    uint8_t            stage;
    float              temp_c;
    float              setpoint_c;
    uint8_t            heat_pct;
    uint8_t            fan_pct;
    uint8_t            fan_state;
    uint16_t           fan_rpm;
    bool               drum_on;
    uint8_t            drum_pct;
    uint32_t           elapsed_s;
    bool               sensor_ok;
    bool               prox_alert;
} roast_status_t;

typedef enum {
    LOG_EVT_START = 0,
    LOG_EVT_ROW,
    LOG_EVT_END,
    LOG_EVT_DUMP,
} log_evt_t;

typedef struct {
    log_evt_t evt;
    uint32_t  t_s;
    float     temp_c;
    float     setpoint_c;
    uint8_t   heat_pct;
    uint8_t   state;
    uint8_t   prox;
    uint16_t  fan_rpm;
    char      profile[16];
} log_entry_t;

#define EVT_FAULT_SENSOR    (1u << 0)
#define EVT_FAULT_OVERTEMP  (1u << 1)
#define EVT_PROX            (1u << 2)
#define EVT_SD_OK           (1u << 3)
#define EVT_FAULT_FAN       (1u << 4)
#define EVT_FAULT_DRUM      (1u << 5)
#define EVT_FAULT_ANY       (EVT_FAULT_SENSOR | EVT_FAULT_OVERTEMP | EVT_FAULT_FAN | EVT_FAULT_DRUM)
