#include "HAL/HAL.h"

#if CONFIG_LIPO_FUEL_GAUGE_ENABLE

#include "BQ27220/BQ27220.h"
static BQ27220 fuel_gauge;
#endif

#define BATT_ADC                    ADC1
#define BATT_MIN_VOLTAGE            3300
#define BATT_MAX_VOLTAGE            4200
#define BATT_FULL_CHARGE_VOLTAGE    4100

#if CONFIG_POWER_BATT_CHG_DET_PULLUP
#  define BATT_CHG_DET_PIN_MODE     INPUT_PULLUP
#  define BATT_CHG_DET_STATUS       (!digitalRead(CONFIG_BAT_CHG_DET_PIN))
#else
#  define BATT_CHG_DET_PIN_MODE     INPUT_PULLDOWN
#  define BATT_CHG_DET_STATUS       ((usage == 100) ? false : digitalRead(CONFIG_BAT_CHG_DET_PIN))
#endif

struct
{
    uint32_t LastHandleTime;
    uint16_t AutoLowPowerTimeout;
    bool AutoLowPowerEnable;
    bool ShutdownReq;
    uint16_t ADCValue;
    HAL::Power_CallbackFunction_t EventCallback;
} Power;

static void Power_ADC_Init(ADC_Type* ADCx)
{
#if CONFIG_LIPO_FUEL_GAUGE_ENABLE
    fuel_gauge.init();
    fuel_gauge.refreshData();
#else
    RCC_APB2PeriphClockCmd(RCC_APB2PERIPH_ADC1, ENABLE);
    RCC_ADCCLKConfig(RCC_APB2CLK_Div8);

    ADC_Reset(ADCx);

    ADC_InitType ADC_InitStructure;
    ADC_StructInit(&ADC_InitStructure);
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanMode = DISABLE;
    ADC_InitStructure.ADC_ContinuousMode = DISABLE;
    ADC_InitStructure.ADC_ExternalTrig = ADC_ExternalTrig_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NumOfChannel = 1;
    ADC_Init(ADCx, &ADC_InitStructure);

    ADC_ClearFlag(ADCx, ADC_FLAG_EC);

    ADC_Ctrl(ADCx, ENABLE);
    ADC_RstCalibration(ADCx);
    while(ADC_GetResetCalibrationStatus(ADCx));
    ADC_StartCalibration(ADCx);
    while(ADC_GetCalibrationStatus(ADCx));
#endif
}

static uint16_t Power_ADC_GetValue()
{
    uint16_t retval = 0;
    if(ADC_GetFlagStatus(BATT_ADC, ADC_FLAG_EC))
    {
        retval = ADC_GetConversionValue(BATT_ADC);
    }
    return retval;
}

static void Power_ADC_Update()
{

#if CONFIG_LIPO_FUEL_GAUGE_ENABLE
    fuel_gauge.refreshData();
#else
    static bool isStartConv = false;

    if(!isStartConv)
    {
        ADC_RegularChannelConfig(
            BATT_ADC,
            PIN_MAP[CONFIG_BAT_DET_PIN].ADC_Channel,
            1,
            ADC_SampleTime_41_5
        );
        ADC_SoftwareStartConvCtrl(BATT_ADC, ENABLE);
        isStartConv = true;
    }
    else
    {
        Power.ADCValue = Power_ADC_GetValue();
        isStartConv = false;
    }
#endif
}

void HAL::Power_Init()
{
    memset(&Power, 0, sizeof(Power));
    Power.AutoLowPowerTimeout = 60;

    Serial.printf("Power: Waiting[%dms]...\r\n", CONFIG_POWER_WAIT_TIME);
    pinMode(CONFIG_POWER_EN_PIN, OUTPUT);
    digitalWrite(CONFIG_POWER_EN_PIN, LOW);
    delay(CONFIG_POWER_WAIT_TIME);
    digitalWrite(CONFIG_POWER_EN_PIN, HIGH);
    Serial.println("Power: ON");

    Power_ADC_Init(BATT_ADC);
    pinMode(CONFIG_BAT_DET_PIN, INPUT_ANALOG);

#if !CONFIG_LIPO_FUEL_GAUGE_ENABLE
    pinMode(CONFIG_BAT_CHG_DET_PIN, BATT_CHG_DET_PIN_MODE);

//    Power_SetAutoLowPowerTimeout(5 * 60);
//    Power_HandleTimeUpdate();
    Power_SetAutoLowPowerEnable(false);
#else
    Power_SetAutoLowPowerTimeout(60);
    Power_HandleTimeUpdate();
    Power_SetAutoLowPowerEnable(true);
#endif
}

void HAL::Power_HandleTimeUpdate()
{
    Power.LastHandleTime = millis();
}

void HAL::Power_SetAutoLowPowerTimeout(uint16_t sec)
{
    Power.AutoLowPowerTimeout = sec;
}

uint16_t HAL::Power_GetAutoLowPowerTimeout()
{
    return Power.AutoLowPowerTimeout;
}

void HAL::Power_SetAutoLowPowerEnable(bool en)
{
    Power.AutoLowPowerEnable = en;
    Power_HandleTimeUpdate();
}

void HAL::Power_Shutdown()
{
    CM_EXECUTE_ONCE(Power.ShutdownReq = true);
}

void HAL::Power_Update()
{
    CM_EXECUTE_INTERVAL(Power_ADC_Update(), 1000);

    if(!Power.AutoLowPowerEnable)
        return;

#if CONFIG_LIPO_FUEL_GAUGE_ENABLE
    if((!fuel_gauge.should_power_off) && fuel_gauge.voltage > BATT_MIN_VOLTAGE)
    {
        Power_HandleTimeUpdate();
    }
#endif

    if(Power.AutoLowPowerTimeout == 0)
        return;

    if(millis() - Power.LastHandleTime >= (Power.AutoLowPowerTimeout * 1000))
    {
        Power_Shutdown();
    }
}

void HAL::Power_EventMonitor()
{
    if(Power.ShutdownReq)
    {
        if(Power.EventCallback)
        {
            Power.EventCallback();
        }
        Backlight_SetGradual(0, 500);
        digitalWrite(CONFIG_POWER_EN_PIN, LOW);
        Serial.println("Power: OFF");
        Power.ShutdownReq = false;
    }
}

void HAL::Power_GetInfo(Power_Info_t* info)
{
#if !CONFIG_LIPO_FUEL_GAUGE_ENABLE
    int voltage = map(
                      Power.ADCValue,
                      0, 4095,
                      0, 3300
                  );

    voltage *= 2;

    CM_VALUE_LIMIT(voltage, BATT_MIN_VOLTAGE, BATT_MAX_VOLTAGE);

    int usage = map(
                    voltage,
                    BATT_MIN_VOLTAGE, BATT_FULL_CHARGE_VOLTAGE,
                    0, 100
                );

    CM_VALUE_LIMIT(usage, 0, 100);

    info->usage = usage;
    info->isCharging = BATT_CHG_DET_STATUS;
    info->voltage = voltage;
#else
    info->voltage = fuel_gauge.voltage;
    info->usage = uint8_t(fuel_gauge.remaining_capacity * 100.0 / fuel_gauge.fullcharge_capacity);
    info->isCharging = (!digitalRead(CONFIG_BAT_CHG_DET_PIN)) && (fuel_gauge.battery_status == BQ27220::CHARGING || fuel_gauge.battery_status == BQ27220::FULL);
    if (info->isCharging)
    {
        info->time_to = fuel_gauge.time_to_full;
    }
    else
    {
        info->time_to = fuel_gauge.time_to_empty;
    }
    info->fullcharge_capacity = fuel_gauge.fullcharge_capacity;
    info->design_capacity = fuel_gauge.design_capacity;
    info->remaining_capacity = fuel_gauge.remaining_capacity;
    info->current = fuel_gauge.current;
    info->average_power = fuel_gauge.average_power;
#endif
}

void HAL::Power_RevertCapacity(uint16_t designCapacity, uint16_t fullChargeCapacity)
{
#if CONFIG_LIPO_FUEL_GAUGE_ENABLE
    fuel_gauge.setCapacity(designCapacity, fullChargeCapacity);
#endif
}

void HAL::Power_SetEventCallback(Power_CallbackFunction_t callback)
{
    Power.EventCallback = callback;
}
