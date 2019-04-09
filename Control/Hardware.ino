/*
 * Copyright (c) 2016-2019 by Pavel Panfilov <firstlast2007@gmail.com> skype pav2000pav
 * &                       by Vadim Kulakov vad7@yahoo.com, vad711
 * "Народный контроллер" для тепловых насосов.
 * Данное програмноое обеспечение предназначено для управления
 * различными типами тепловых насосов для отопления и ГВС.
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 */
// --------------------------------------------------------------------------------
// Описание базовых классов для работы c "железом"
// (датчики и исполнительные устройства) зависит от контроллера
// --------------------------------------------------------------------------------
#include "Hardware.h"

// --------------------------------------------------------------------------------
// Настройка таймера и ацп для чтения датчика давления
// Зависит от чипа
// --------------------------------------------------------------------------------
// Старт считывания АЦП
void start_ADC()
{
	adc_setup();                                       // setup ADC
	pmc_enable_periph_clk(TC_INTERFACE_ID + 0 * 3 + 0);    // clock the TC0 channel 0

	TcChannel * t = &(TC0->TC_CHANNEL)[0];              // pointer to TC0 registers for its channel 0
	t->TC_CCR = TC_CCR_CLKDIS;                          // disable internal clocking while setup regs
	t->TC_IDR = 0xFFFFFFFF;                             // disable interrupts
	t->TC_SR;                                           // read int status reg to clear pending
	t->TC_CMR = TC_CMR_TCCLKS_TIMER_CLOCK1 |             // use TCLK1 (prescale by 2, = 42MHz)
			TC_CMR_WAVE |                            // waveform mode
			TC_CMR_WAVSEL_UP_RC |                    // count-up PWM using RC as threshold
			TC_CMR_EEVT_XC0 |                        // Set external events from XC0 (this setup TIOB as output)
			TC_CMR_ACPA_CLEAR | TC_CMR_ACPC_SET |    // set clear and set from RA and RC compares
			TC_CMR_BCPB_NONE | TC_CMR_BCPC_NONE;

	t->TC_RC = SystemCoreClock / 2 / ADC_FREQ;        // counter resets on RC, so sets period in terms of 42MHz clock
	t->TC_RA = SystemCoreClock / 2 / ADC_FREQ / 2;     // roughly square wave
	t->TC_CCR = TC_CCR_CLKEN | TC_CCR_SWTRG;  // re-enable local clocking and switch to hardware trigger source.

}

// Установка АЦП
void adc_setup()
{
	uint16_t adcMask = 0;
	uint8_t max = 0;
#ifdef VCC_CONTROL                             // если разрешено чтение напряжение питания
	adcMask |= (1<<PIN_ADC_VCC);               // Добавить маску контроля питания
	max = PIN_ADC_VCC;
#endif
	//   adcMask=adcMask|(0x1u<<ADC_TEMPERATURE_SENSOR);         // добавить маску для внутреннего датчика температуры
	//   adc_enable_ts(ADC);                                     // разрешить чтение температурного датчика в регистре ADC Analog Control Register

	// Расчет маски каналов
	for(uint8_t i = 0; i < ANUMBER; i++) {   // по всем датчикам
		if(HP.sADC[i].get_present() && !HP.sADC[i].get_fmodbus()) {
			if(max < HP.sADC[i].get_pinA()) max = HP.sADC[i].get_pinA();
			adcMask |= 1 << HP.sADC[i].get_pinA();
		}
	}
	NVIC_EnableIRQ(ADC_IRQn);        // enable ADC interrupt vector
	ADC->ADC_IDR = 0xFFFFFFFF;       // disable interrupts IDR Interrupt Disable Register
	ADC->ADC_IER = 1 << max;         // Самый старший канал
	ADC->ADC_CHDR = 0xFFFF;          // Channel Disable Register CHDR disable all channels
	ADC->ADC_CHER = adcMask;         // Channel Enable Register CHER enable just A11  каналы здесь SAMX3!!
	ADC->ADC_CGR = 0x15555555;       // //0x55555555 All gains set to x1 Channel Gain Register
	ADC->ADC_COR = 0x00000000;       // All offsets off Channel Offset Register
	// 12bit, 14MHz, trig source TIO from TC0
	ADC->ADC_MR = ADC_MR_PRESCAL(ADC_PRESCAL) | ADC_MR_LOWRES_BITS_12 | ADC_MR_USEQ_NUM_ORDER | ADC_MR_STARTUP_SUT16 | ADC_MR_TRACKTIM(16) | ADC_MR_SETTLING_AST17 | ADC_MR_TRANSFER(2) | ADC_MR_TRGSEL_ADC_TRIG1 | ADC_MR_TRGEN;
	adc_set_bias_current(ADC, 0);    // for sampling frequency: 0 - below 500 kHz, 1 - between 500 kHz and 1 MHz.
}


// Обработчик прерывания, как можно короче
#ifdef __cplusplus
extern "C"
{
#endif
void ADC_Handler(void)
{
#ifdef VCC_CONTROL  // если разрешено чтение напряжение питания
	HP.AdcVcc = (uint32_t)(*(ADC->ADC_CDR + PIN_ADC_VCC));
#endif
	for(uint8_t i = 0; i < ANUMBER; i++)    // по всем датчикам
	{
		sensorADC *adc = &HP.sADC[i];
#ifdef ADC_SKIP_EXTREMUM
		int32_t a = ADC->ADC_CDR[adc->get_pinA()]; // get conversion result
		if(adc->adc_lastVal != 0xFFFF && abs(a - adc->adc_lastVal) > ADC_SKIP_EXTREMUM) {
			adc->adc_lastVal = 0xFFFF;
			continue;
		}
		adc->adc_lastVal = a;
#else
		adc->adc_lastVal = (uint32_t)ADC->ADC_CDR[adc->get_pinA()];  // get conversion result
#endif
		// Усреднение значений
		adc->adc_sum = adc->adc_sum + adc->adc_lastVal - adc->adc_filter[adc->adc_last];   // Добавить новое значение, Убрать самое старое значение
		adc->adc_filter[adc->adc_last] = adc->adc_lastVal;			                       // Запомнить новое значение
		if(adc->adc_last < adc->adc_filter_max) adc->adc_last++;
		else {
			adc->adc_last = 0;
			adc->adc_flagFull = true;
		}
	}
	// if (ADC->ADC_ISR & (1<<ADC_TEMPERATURE_SENSOR))   // ensure there was an End-of-Conversion and we read the ISR reg
	//            HP.AdcTempSAM3x =(unsigned int)(*(ADC->ADC_CDR+ADC_TEMPERATURE_SENSOR));   // если готов прочитать результат
}

#ifdef __cplusplus
}
#endif
    
// ------------------------------------------------------------------------------------------
// Аналоговые датчики давления --------------------------------------------------------------
// Давление хранится в СОТЫХ БАР
void sensorADC::initSensorADC(uint8_t sensor, uint8_t pinA, uint16_t filter_size)
{
	// Инициализация структуры для хранения "сырых"данных с аналогового датчика.
	if(SENSORPRESS[sensor]) adc_filter_max = filter_size;   // отводим память если используем датчик под сырые данные
	else adc_filter_max = 1;
	adc_filter = (uint16_t*) malloc(sizeof(uint16_t) * adc_filter_max);
	if(adc_filter == NULL) {   // ОШИБКА если память не выделена
		set_Error(ERR_OUT_OF_MEMORY, (char*) "sensorADC");
		return;
	}
	memset(adc_filter, 0, sizeof(uint16_t) * adc_filter_max);
	adc_filter_max--;
	adc_sum = 0;                                                                   // сумма
	adc_last = 0;                                                                  // текущий индекс
	adc_flagFull = false;                                                          // буфер полный
	adc_lastVal = 0;                                                               // последнее считанное значение
	clearBuffer();

	testMode = NORMAL;                           // Значение режима тестирования
	cfg.minPress = MINPRESS[sensor];                 // минимально разрешенное давление
	cfg.maxPress = MAXPRESS[sensor];                 // максимально разрешенное давление
	cfg.testPress = TESTPRESS[sensor];               // Значение при тестировании
	cfg.zeroPress = ZEROPRESS[sensor];               // отсчеты АЦП при нуле датчика
	cfg.transADC = TRANsADC[sensor];                 // коэффициент пересчета АЦП в давление
	cfg.number = sensor;
	pin = pinA;
	flags = SENSORPRESS[sensor] << fPresent;	 // наличие датчика
#ifdef ANALOG_MODBUS
	flags |= (ANALOG_MODBUS_ADDR[sensor] != 0)<<fsensModbus;  // Дистанционный датчик по модбас
#endif
	Chart.init(sensor <= PCON ? SENSORPRESS[sensor] : false);  // инициалазация статистики
	err = OK;                                     // ошибка датчика (работа)
	Press = 0;                                    // давление датчика (обработанная)
	Temp = ERROR_TEMPERATURE;
	note = (char*) notePress[sensor];              // присвоить наименование датчика
	name = (char*) namePress[sensor];              // присвоить имя датчика
}
    
 // очистить буфер АЦП
 void sensorADC::clearBuffer()
 {
#if P_NUMSAMLES > 1
      for(uint16_t i=0;i<P_NUMSAMLES;i++) p[i]=0;         // обнуление буффера значений
      sum=0;
      last=0;
#endif
      SETBIT0(flags,fFull);                      // Буфер не полный
      lastPress=0;                               // последнее считанное давление по умолчанию ноль
 }
  
// чтение данных c аналогового датчика (АЦП) возвращает код ошибки, делает все преобразования
 int8_t  sensorADC::Read()
 {

	 if(!(GETBIT(flags,fPresent)))  return err;        // датчик запрещен в конфигурации ничего не делаем

	 if (testMode!=NORMAL) lastPress=cfg.testPress;        // В режиме теста
	 else                                              // Чтение датчика
	 {
#ifdef DEMO
		 lastADC=random(1350,2500);                   // В демо режиме генерим значение
#else
	#ifdef ANALOG_MODBUS
		 if(get_fmodbus()) {
			for(uint8_t i = 0; i < ANALOG_MODBUS_NUM_READ; i++) {
				err = Modbus.readHoldingRegisters16(ANALOG_MODBUS_ADDR[cfg.number], ANALOG_MODBUS_REG[cfg.number] - 1, &adc_lastVal);
				if(err == OK) {
					lastADC = adc_lastVal;
					break;
				}
				_delay(ANALOG_MODBUS_ERR_DELAY);
			}
			if(err) {
				journal.jprintf(pP_TIME, "Error read %s by Modbus: %d\n", name, err);
				set_Error(ERR_READ_PRESS, name);
				return ERR_READ_PRESS;
			}
		 } else
	#endif
		 {
			 if(adc_flagFull) lastADC=adc_sum/(adc_filter_max+1); else lastADC=adc_sum/adc_last;
			 //if(adc.error!=OK)  {err=ERR_READ_PRESS;set_Error(err,name);return err;}   // Проверка на ошибку чтения ацп
		 }
#endif
		 lastPress=(int)((float)lastADC*(cfg.transADC))-cfg.zeroPress;
	 }
	 //  Serial.print(lastADC);  Serial.print(" ");  Serial.println(lastPress);
#if P_NUMSAMLES > 1
	 // Усреднение значений
	 sum=sum+lastPress;          // Добавить новое значение
	 sum=sum-p[last];            // Убрать самое старое значение
	 p[last]=lastPress;          // Запомить новое значение
	 if (last<P_NUMSAMLES-1) last++; else {last=0; SETBIT1(flags,fFull);}
	 if (GETBIT(flags,fFull)) Press=sum/P_NUMSAMLES; else Press=sum/last;
#else
	 Press = lastPress;
#endif
	 Temp = ERROR_TEMPERATURE;

	 // Проверка на ошибки именно здесь обрабатывются ошибки и передаются на верх
	 // Берутся МНОВЕННЫЕ значения!!!! для увеличения реакции системы на ошибки
	 // При ошибке запоминаем мговенное значение как среднее  что бы видно было
	 if(lastPress<cfg.minPress)  { err=ERR_MINPRESS;set_Error(err,name); return err;}
	 if(lastPress>cfg.maxPress)  { err=ERR_MAXPRESS;set_Error(err,name); return err;}

	 // Дошли до сюда значит ошибок нет
	 err=OK;                                         // Новый цикл новые ошибки
	 return err;
 }

//// полный цикл получения данных возвращает значение давления, только тестирование!! никакие переменные класса не трогает!!
//int16_t sensorADC::Test()
//{
//   int16_t x;
//   if (adc.flagFull) x=adc.sum/adc.filter_size; else x=adc.sum/adc.last;
//   return (int)((float)x*(transADC))-zeroPress;
//}

// Установка 0 датчика темпеартуры
int8_t sensorADC::set_zeroPress(int16_t p)
{
  if((p>=0)&&(p<=4096)) { clearBuffer(); cfg.zeroPress=p; return OK;} // Суммы обнулить надо
  else return WARNING_VALUE;
}

//Получить значение давления датчика в сотых бара
int16_t sensorADC::get_Press()
{
if (!(GETBIT(flags,fPresent))) return -100;                  // датчик запрещен в конфигурации то давление -100
return Press;    
}

// Установить значение коэффициента преобразования напряжение (отсчеты ацп)-температура
int8_t sensorADC::set_transADC(float p)
{
  if((p>=0.0)&&(p<=4.0)) { clearBuffer(); cfg.transADC=p; return OK;}  // Суммы обнулить надо
  else return WARNING_VALUE;
}

// Установить значение давления датчика в режиме теста
int8_t sensorADC::set_testPress(int16_t p)            
{
	cfg.testPress=p;
	return OK;
}

// ------------------------------------------------------------------------------------------
// Цифровые контактные датчики (есть 2 состяния 0 и 1) --------------------------------------

// Инициализация контактного датчика
void  sensorDiditalInput::initInput(int sensor)
{
   Input=false;                    // Состояние датчика
   number = sensor;
   testInput=TESTINPUT[sensor];    // Состояние датчика в режиме теста
   testMode=NORMAL;                // Значение режима тестирования
   alarmInput=ALARMINPUT[sensor];  // Состояние датчика в режиме аварии
   err=OK;                         // ошибка датчика (работа) при ошибке останов ТН
   flags=0x00;                     // сброс флагов
   // флаги  0 - наличие датчика,  1- режим теста
   SETBIT1(flags,fPresent);        // наличие датчика в текушей конфигурации
   type=SENSORTYPE[sensor];         // тип датчика
   pin=pinsInput[sensor];           // пин датчика
   pinMode(pin, INPUT);             // Настроить ножку на вход
   note=(char*)noteInput[sensor];   // присвоить наименование датчика
   name=(char*)nameInput[sensor];   // присвоить имя датчика
 //  Read();                        // Нельзя читать до тех пор пока не ЗАГРУЖЕНЫ правильные настройки - можно уйти в аварию
   Input=digitalReadDirect(pin);    // Состояние датчика прочитать датчик но не анализировать аварию (хотя этого можно не делать)
};

// Чтение датчика возвращает ошибку или ОК
int8_t sensorDiditalInput::Read(boolean fast)
{
	err = OK;                                            // Ошибки сбросить
	if(testMode != NORMAL) Input = testInput;            // В режиме теста
	else {
		boolean in = digitalReadDirect(pin);
		if(!fast && in != Input) {
			uint8_t i;
			for(i = 0; i < 2; i++) {
				_delay(1);
				if(in != digitalReadDirect(pin)) break;
			}
			if(i == 2) Input = in;
		}
	}
	if(type == pALARM && Input == alarmInput)     // Срабатывание аварийного датчика (только его!)
	{
		err = ERR_DINPUT;
		set_Error(err, name);
	}
	return err;
}
    
// Установить Состояние датчика в режиме теста
int8_t sensorDiditalInput::set_testInput( int16_t i)         
{
   if(i==1)  { testInput=true; return OK;} 
    else  if (i==0)  { testInput=false; return OK;} 
     else return WARNING_VALUE;
}
 
// Установить Состояние датчика в режиме аварии
int8_t sensorDiditalInput::set_alarmInput( int16_t i)         
{
  if(i==1)  { alarmInput=true; return OK;} 
    else  if(i==0)  { alarmInput=false; return OK;} 
     else return WARNING_VALUE;
}

// ------------------------------------------------------------------------------------------
// Цифровые частотные датчики (значение кодируется в выходной частоте) ----------------------
// основное назначение - датчики потока
// Частота кодируется в тысячных герца
// Число импульсов рассчитывается за базовый период (BASE_TIME), т.к частоты малы период надо савить не менее 5 сек

// Обработчики прерываний для подсчета частоты
void InterruptFLOWCON()  
{
#ifdef FLOWCON  
  HP.sFrequency[FLOWCON].InterruptHandler();
#endif  
}
void InterruptFLOWEVA()  
{
#ifdef FLOWEVA   
  HP.sFrequency[FLOWEVA].InterruptHandler();
#endif  
}
#ifdef FLOWPCON  
void InterruptFLOWPCON() 
{
  HP.sFrequency[FLOWPCON].InterruptHandler();
}
#endif  

// Инициализация частотного датчика, на входе номер сенсора по порядку
void sensorFrequency::initFrequency(int sensor)                     
{
   number = sensor;
   Frequency=0;                                   // значение частоты
   Value=0;                                       // значение датчика в ТЫСЯЧНЫХ (умножать на 1000)
   Capacity=HEAT_CAPACITY;                        // значение теплоемкости теплоносителя в конутре где установлен датчик [Cp, Дж/(кг·град)]
   minValue=MINFLOW[sensor];                      // минимальное значение датчика
   testValue=TESTFLOW[sensor];                    // Состояние датчика в режиме теста
   kfValue=TRANSFLOW[sensor];                     // коэффициент пересчета частоты в значение
   testMode=NORMAL;                               // Значение режима тестирования
   count=0;                                       // число импульсов за базовый период (то что меняется в прерывании)
   err=OK;                                        // ошибка датчика (работа) при ошибке останов ТН
   flags=0x00;                                    // Обнулить флаги
   SETBIT1(flags,fPresent);                       // наличие датчика в текушей конфигурации - датчик всегда есть в концигурвции добавлено для единообразия
   pin=pinsFrequency[sensor];                     // Ножка куда прицеплен датчик
   note=(char*)noteFrequency[sensor];             // наименование датчика
   name=(char*)nameFrequency[sensor];             // Имя датчика
   Chart.init(true);                              // инициалазация статистики
   reset();
   // Привязывание обработчика преваний к методу конкретного класса
   //   LOW вызывает прерывание, когда на порту LOW
   //   CHANGE прерывание вызывается при смене значения на порту, с LOW на HIGH и наоборот
   //   RISING прерывание вызывается только при смене значения на порту с LOW на HIGH
   //   FALLING прерывание вызывается только при смене значения на порту с HIGH на LOW
        if (sensor==FLOWCON)  attachInterrupt(pin,InterruptFLOWCON,CHANGE); 
#ifdef FLOWEVA        
   else if (sensor==FLOWEVA)  attachInterrupt(pin,InterruptFLOWEVA,CHANGE); 
#endif   
#ifdef FLOWPCON   
   else if (sensor==FLOWPCON) attachInterrupt(pin,InterruptFLOWPCON,CHANGE);
#endif     
   else err=ERR_NUM_FREQUENCY;
}

// Получить (точнее обновить) значение датчика, возвращает 1, если новое значение рассчитано
int8_t sensorFrequency::Read()
{
	if(testMode != NORMAL) {
		Value = testValue;
#if defined(RPUMPI) && defined(FLOWEVA)
		if(number == FLOWEVA && !HP.dRelay[RPUMPI].get_Relay()) Value = 0;
#endif
#if defined(RPUMPO) && defined(FLOWCON)
		if(number == FLOWCON && !HP.dRelay[RPUMPO].get_Relay()) Value = 0;
#endif
		Frequency = Value * kfValue / 360;
		return 0;
	}   // В режиме теста
#ifdef DEMO
	Frequency=random(2500,9000);
	count=0;
	//   Value=60.0*Frequency/kfValue/1000.0;                  // переводим в Кубы в час  (Frequency/kfValue- литры в минуту)  watt=(Value/3.600) *4.191*dT
	Value=Frequency * 360 / kfValue;// ЛИТРЫ В ЧАС (ИЛИ ТЫСЯЧНЫЕ КУБА) частота в тысячных, и коэффициент правим
	//  journal.jprintf("Sensor %s: frequence=%.3f flow=%.3f\n",name,Frequency/(1000.0),Value/(1000.0));
#else
	if(GetTickCount() - sTime >= (uint32_t)BASE_TIME_READ * 1000) {  // если только пришло время измерения
		uint32_t tickCount, cnt;
		//if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) taskENTER_CRITICAL();
		tickCount = GetTickCount();
		cnt = count;
		count = 0;
		//if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) taskEXIT_CRITICAL();
		__asm__ volatile ("" ::: "memory");
		Frequency = (cnt * 500 * 1000) / (tickCount - sTime); // ТЫСЯЧНЫЕ ГЦ время в миллисекундах частота в тысячных герца *2 (прерывание по обоим фронтам)!!!!!!!!
		sTime = tickCount;
		//   Value=60.0*Frequency/kfValue/1000.0;               // Frequency/kfValue  литры в минуту а нужны кубы
		//       Value=((float)Frequency/1000.0)/((float)kfValue/360000.0);          // ЛИТРЫ В ЧАС (ИЛИ ТЫСЯЧНЫЕ КУБА) частота в тысячных, и коэффициент правим
		Value = Frequency * 360 / kfValue; // ЛИТРЫ В ЧАС (ИЛИ ТЫСЯЧНЫЕ КУБА) частота в тысячных, и коэффициент правим
		return 1;
	}
#endif
	return 0;
}

void sensorFrequency::reset(void)
{

	//if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) taskENTER_CRITICAL();
	sTime = GetTickCount();
	count = 0;
	//if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) taskEXIT_CRITICAL();
}

// Установить Состояние датчика в режиме теста
int8_t  sensorFrequency::set_testValue(int16_t i) 
{
   testValue=i;
   return OK;
}
// Установить теплоемкость больше 5000 не устанавливается
int8_t sensorFrequency::set_Capacity(uint16_t c)                      
{  
  if (c<=5000) {Capacity=c; return OK;} else return WARNING_VALUE;    
}   

	// Установить минимальное значение датчика
void sensorFrequency::set_minValue(float f)
{
	minValue = rd(f, 10);
}

// ------------------------------------------------------------------------------------------
// Исполнительное устройство РЕЛЕ (есть 2 состяния 0 и 1) --------------------------------------
// Relay = true - это означает включение исполнительного механизама. 
// При этом реальный выход и состояние (физическое реле) определяется дефайнами RELAY_INVERT и RTRV_INVERT
// ВНИМАНИЕ: По умолчанию (не определен RELAY_INVERT) выход инвертируется - Влючение реле (Relay=true) соответствует НИЗКИЙ уровень на выходе МК
void devRelay::initRelay(int sensor)
{
   flags=0x00;
   number = sensor;
   testMode=NORMAL;                // Значение режима тестирования
   flags=0x01;                     // наличие датчика в текушей конфигурации (отстатки прошлого, реле сейчас есть всегда)  флаги  0 - наличие датчика,  1- режим теста
   pin=pinsRelay[sensor];  
   pinMode(pin, OUTPUT);           // Настроить ножку на выход
   Relay=false;                    // Состояние реле - выключено
#ifndef RELAY_INVERT            // Нет инвертирования реле -  Влючение реле (Relay=true) соответсвует НИЗКИЙ уровень на выходе МК
	#ifdef RTRV_INVERT              // Признак инвертирования 4х ходового
   	   digitalWriteDirect(pin, number != RTRV);  // Установить значение
	#else
   	   digitalWriteDirect(pin, true);  // Установить значение
	#endif
#else
	#ifdef RTRV_INVERT              // Признак инвертирования 4х ходового
   	   digitalWriteDirect(pin, number == RTRV);  // Установить значение
	#else
   	   digitalWriteDirect(pin, false);  // Установить значение
	#endif
#endif
   note=(char*)noteRelay[sensor];  // присвоить описание реле
   name=(char*)nameRelay[sensor];  // присвоить имя реле
}


// Установить реле в состояние r, базовая функция все остальные функции используют ее
// Если состояния совпадают то ничего не делаем, 0/-1 - выкл основной алгоритм, fR_Status* - включить, -fR_Status* - выключить)
int8_t devRelay::set_Relay(int8_t r)
{
	if(!(flags & (1 << fPresent))) return ERR_DEVICE;  // Реле не установлено  и пытаемся его включить
	if(r == 0) r = -fR_StatusMain;
	else if(r == fR_StatusAllOff) {
		flags &= ~fR_StatusMask;
		r = -fR_StatusMain;
	}
	flags = (flags & ~(1 << abs(r))) | ((r > 0) << abs(r));
	r = (flags & fR_StatusMask) != 0;
	if(Relay == r) return OK;   // Ничего менять не надо выходим
    Relay = r;                  // Все удачно, сохранить
	if(testMode == NORMAL || testMode == HARD_TEST || (testMode == TEST && number != RCOMP)) {
#ifndef RELAY_INVERT            // Нет инвертирования реле -  Влючение реле (Relay=true) соответсвует НИЗКИЙ уровень на выходе МК
		r = !r;
#endif
#ifdef RTRV_INVERT              // Признак инвертирования 4х ходового
		if(number == RTRV) r = !r;
#endif
		digitalWriteDirect(pin, r);
	}
#ifdef RELAY_WAIT_SWITCH
	uint8_t tasks_suspended = TaskSuspendAll();
	delay(RELAY_WAIT_SWITCH);
	if(tasks_suspended) xTaskResumeAll();
#endif
	journal.jprintf(pP_TIME, "Relay %s: %s\n", name, Relay ? "ON" : "OFF");
	return OK;
}

// ------------------------------------------------------------------------------------------
// ЭРВ ТОЛЬКО ОДНА ШТУКА ВСЕГДА (не массив) ---------------------------------------- --------
 #ifdef EEV_DEF     
const char *noteEEV = {"Электронно регулируемый вентиль" };// Описание
const char *nameEEV = {"EEV"} ;  //  Имя
// Инициализация ЭРВ
void devEEV::initEEV()
{
  EEV=-1;                               // шаговик в непонятном положении
  setZero=false;                        // Признак процесса обнуления (шаговик ищет 0)
  err=OK;                               // Ошибок нет
  Resume(); 			                // Обнулить рабочие переменные
  testMode=NORMAL;                      // Значение режима тестирования
	
// Устновка настроек по умолчанию (структара данных _data)
 _data.tOverheat = DEFAULT_OVERHEAT;                 // Перегрев ЦЕЛЬ (сотые градуса)
 _data.pid_time = DEFAULT_EEV_TIME;                  // Постоянная интегрирования времени в секундах ЭРВ СЕКУНДЫ
 _data.pid.Kp =  -DEFAULT_EEV_Kp * 10;               // ПИД Коэф пропорц, в тысячных
 _data.pid.Ki =  -DEFAULT_EEV_Ki * 10;               // ПИД Коэф интегр.,  в тысячных
 _data.pid.Kd =  -DEFAULT_EEV_Kd * 10;               // ПИД Коэф дифф., в тысячных
 _data.Correction = 0;                               // 0.855 ПЕРЕДЕЛАНО  зона не чуствительности перегрева в "плюсе" в этой зоне на каждом шаге эрв закрывается на 1 шаг
 _data.manualStep = (EEV_STEPS-_data.minSteps)/2+_data.minSteps;  // Число шагов открытия ЭРВ для правила работы ЭРВ «Manual» - половина диапазона ЭРВ
 _data.typeFreon = DEFAULT_FREON_TYPE;               // Тип фреона
 _data.ruleEEV = DEFAULT_RULE_EEV;                   // правило работы ЭРВ
#ifdef DEF_OHCor_OverHeatStart						 // Корректировка перегрева
 _data.OHCor_Delay = DEF_OHCor_Delay;			     // Задержка после старта компрессора, сек
 _data.OHCor_TDIS_TCON = DEF_OHCor_TDIS_TCON;		 // Температура нагнетания - конденсации при 30С и 0 конденсации
 _data.OverheatMin = DEF_OHCor_OverHeatMin;		     // Минимальный перегрев (сотые градуса)
 _data.OverheatMax = DEF_OHCor_OverHeatMax;		     // Максимальный перегрев (сотые градуса)
 _data.OverHeatStart = DEF_OHCor_OverHeatStart; 	 // Начальный перегрев (сотые градуса)
 _data.OHCor_Period = DEF_OHCor_Period;
#endif
 _data.speedEEV = DEFAULT_SPEED_EEV;                 // Скорость шагового двигателя ЭРВ (импульсы в сек.)
 _data.preStartPos = DEFAULT_PRE_START_POS;          // ПУСКОВАЯ позиция ЭРВ (ТО что при старте компрессора ПРИ РАСКРУТКЕ)
 _data.StartPos = DEFAULT_START_POS;                 // СТАРТОВАЯ позиция ЭРВ после раскрутки компрессора т.е. ПОЗИЦИЯ С КОТОРОЙ НАЧИНАЕТСЯ РАБОТА проходит DelayStartPos сек
 _data.minSteps = EEV_CLOSE_STEP;                    // Минимальное число шагов открытия ЭРВ
 _data.maxSteps = EEV_STEPS;                         // Максимальное число шагов ЭРВ (диапазон)
 _data.trend_threshold = 3;
 _data.tOverheatTCOMP = 850;
 _data.tOverheatTCOMP_delta = 300;
 _data.PosAtHighTemp = 10;
 _data.pid2_delta = 070;

  // ЭРВ Времена и задержки
 _data.delayOnPid = DEFAULT_DELAY_ON_PID;             // Задержка включения EEV после включения компрессора (сек).  Точнее после выхода на рабочую позицию Общее время =delayOnPid+DelayStartPos
 _data.delayOn = DEFAULT_DELAY_ON;                    // Задержка между открытием (для старта) ЭРВ и включением компрессора, для выравнивания давлений (сек). Если ЭРВ закрывлось при остановке
 _data.DelayStartPos = DEFAULT_DELAY_START_POS;       // Время после старта компрессора когда EEV выходит на стартовую позицию - облегчение пуска вначале ЭРВ
 _data.delayOff = DEFAULT_DELAY_OFF;                  // Задержка закрытия EEV после выключения насосов (сек). Время от команды стоп компрессора до закрытия ЭРВ = delayOffPump+delayOff
 _data.flags = 0x00;                                  // флаги ЭРВ,
 #ifdef EEV_DEF 
  SETBIT1(_data.flags,fPresent);                      // наличие ЭРВ в текушей конфигурации
 #endif 
  if (DEFAULT_HOLD_MOTOR) SETBIT1(_data.flags,fHoldMotor);
  SETBIT0(_data.flags,fCorrectOverHeat);              // Включен режим корректировки перегрева
  SETBIT1(_data.flags,fOneSeekZero);                  //  Флаг однократного поиска "0" ЭРВ (только при первом включении ТН)
  SETBIT0(_data.flags,fEevClose);                     // Флаг закрытие ЭРВ при выключении компрессора
  SETBIT0(_data.flags,fLightStart);                   // флаг Облегчение старта компрессора   приоткрытие ЭРВ в момент пуска компрессора
  SETBIT1(_data.flags,fStartFlagPos);                 // флаг Всегда начинать работу ЭРВ со стратовой позици
  SETBIT1(_data.flags,fEEV_StartPosByTemp);

  Chart.init(get_present());                   // инициалазация статистики
  name=(char*)nameEEV;                  // Присвоить имя
  note=(char*)noteEEV;                  // Присвоить описание
  
// Инициализация шагового двигателя ЭРВ   ВАЖНО ПРАВИЛЬНОЕ ПОДКЛЮЧЕНИЕ!!!
// Рабочие комбинации для подключения шаговика на 5 вольт 28BYJ-48
// So in the end, four entries worked: (8,10,11,9), (9,11,8,10), (10,8,9,11), and (11,9,10,8).
// http://www.utopiamechanicus.com/article/arduino-stepper-motor-setup-troubleshooting/
// Подключение двигателя 28BYJ-48. Драйвер ULN-2003 схема (8,10,11,9)
// http://42bots.com/tutorials/28byj-48-stepper-motor-with-uln2003-driver-and-arduino-uno/
      // ШАГОВИК на 5 вольт 28BYJ-48 5V  -----------------
      // нога 1 синий       - В
      // нога 2 фиолетовый  + А
      // нога 3 желтый      + В
      // нога 4 оранжевый   - А
      // нога 5 красный     + общий
      // ЭРВ 12 вольт ------------------------------------
      // нога 1 оранжевый  + А
      // нога 2 красный    + В
      // нога 3 желтый     - А
      // нога 4 черный     - В
      // нога 5 синий      + общий
      // Распиновка LM9333 -------------------------------
      // D24     нога 1
      // D26     нога 2
      // D25     нога 3
      // D27     нога 4
      // Распиновка ULN2003 -------------------------------
      // D24     нога 1
      // D25     нога 2
      // D26     нога 3
      // D27     нога 4
      // Инициализация библиотеки порядок указания фаз в функции initStepMotor +A +B -A -B
    
#ifdef DEMO
  stepperEEV.initStepMotor(_data.maxSteps, PIN_EEV3_D26,PIN_EEV2_D25,PIN_EEV4_D27,PIN_EEV1_D24);          // для тестирования  на шаговике 5 вольт вроде работает
#else  
    #ifdef DRV_EEV_L9333                                                                          // использование драйвера L9333
      #ifdef  EEV_INVERT                                                                          // Признак инвертирования движения ЭРВ
         stepperEEV.initStepMotor(_data.maxSteps,PIN_EEV4_D27,PIN_EEV2_D25,PIN_EEV3_D26,PIN_EEV1_D24);    // на 8 фазном работает 480 шагов обратное подключение
      #else
         stepperEEV.initStepMotor(_data.maxSteps,PIN_EEV1_D24,PIN_EEV3_D26,PIN_EEV2_D25,PIN_EEV4_D27);    // на 8 фазном работает 480 шагов прямое подключение
      #endif
    #else    
      #ifdef  EEV_INVERT                                                                          // Признак инвертирования движения ЭРВ
         stepperEEV.initStepMotor(_data.maxSteps,PIN_EEV4_D27,PIN_EEV3_D26,PIN_EEV2_D25,PIN_EEV1_D24);    // на 8 фазном работает 480 шагов обратное подключение
      #else
         stepperEEV.initStepMotor(_data.maxSteps,PIN_EEV1_D24,PIN_EEV2_D25,PIN_EEV3_D26,PIN_EEV4_D27);    // на 8 фазном работает 480 шагов прямое подключение
      #endif
    #endif  // DRV_EEV_L9333
#endif // DEMO   
stepperEEV.setSpeed(_data.speedEEV);   // Установить скорость движения
//journal.jprintf(" EEV init: OK\r\n"); 
}

// Восстановление слежения ЭРВ, если конечно задача запущена
void devEEV::Resume()
{
  fPause=false;                          // не пауза
  resetPID();                            // Cброс служебных переменных ПИД
}
 
// Запустить ЭРВ - возвращает ок или код ошибки
// На стартовую позицию не выводит
int8_t devEEV::Start()
{
	Resume();
	//  EEV=0;
	err = OK;                               // Ошибок нет
	if(!GETBIT(_data.flags, fPresent)) {
		journal.jprintf(" EEV is not available\n");
		return err;
	}  // если ЭРВ нет то ничего не делаем
	if((!GETBIT(_data.flags, fOneSeekZero)) || (GETBIT(_data.flags,fOneSeekZero) && (EEV < 0))) { // есть вариант однократного поиска "0" ЭРВ
		set_zero();
	}                          // установить 0
	//  journal.jprintf(" EEV set StartPos: %d\n",StartPos);
	//  set_EEV(StartPos);      // Выставить положение ЭРВ - StartPos
	return OK;
}
// Гарантированно (шагов больше чем диапазон) закрыть ЭРВ возвращает код ошибки
int8_t devEEV::set_zero()
{
	if(!setZero) {
		journal.jprintf(" EEV: Set zero\n");
		setZero = true;                                             // Признак ПРОЦЕССА обнуления счетчика шагов EEV  Ставить в начале!!
		EEV = -1;
		if(testMode != SAFE_TEST) stepperEEV.step(-_data.maxSteps - EEV_SET_ZERO_OVERRIDE);  // не  SAFE_TEST - работаем
		else EEV = 0;                                               // SAFE_TEST только координаты меняем
	}
	return OK;
}

 // Перейти на позицию абсолютную возвращает код ошибки
 // Отрицательные значения ЛЮБЫЕ признак установки 0
int8_t devEEV::set_EEV(int16_t x)
{
  err=OK;
  if (!(GETBIT(_data.flags,fPresent)))  { err=ERR_DEVICE; return err;   }    // ЭРВ не установлен
  if(x < EEV_CLOSE_STEP) x = EEV_CLOSE_STEP;
  else if(x > _data.maxSteps) x = _data.maxSteps;
  if(testMode!=SAFE_TEST) stepperEEV.step(x);                   // не  SAFE_TEST - работаем
  else EEV=x;                                                    // SAFE_TEST только координаты меняем
 return err;  
}

// Стартовая позиция ЭРВ
uint16_t devEEV::get_StartPos()
{
	if(GETBIT(_data.flags, fEEV_StartPosByTemp)) {
		int16_t t = HP.sTemp[HP.get_modWork() == pCOOL || HP.get_modWork() == pNONE_C ? TEVAOUTG : TCONOUTG].get_Temp();
		if(t <= EEV_START_POS_LOW_TEMP) return _data.StartPos;
		else if(t >= EEV_START_POS_HIGH_TEMP) return _data.PosAtHighTemp;
		return (int32_t)_data.StartPos - (t - EEV_START_POS_LOW_TEMP) * (_data.StartPos - _data.PosAtHighTemp) / (EEV_START_POS_HIGH_TEMP - EEV_START_POS_LOW_TEMP);
	} else return _data.StartPos;
}

// Вычислить текущий перегрев, вычисляется каждое измерение (опрос датчиков)
// На входе текущие параметры датчиков, для всех вариантов на входе  TEVAOUT,TEVAIN, Press
// Если датчик давления отсутствует до давление будет -1000, и по этому опредяляем его наличие в конфигурации и как определять перегрев
// Если ЭРВ запрещена в конфигурации, то перегрев не вычисляется =0 и сразу выходим
int16_t devEEV::set_Overheat(boolean fHeating) // int16_t rto,int16_t out, int16_t in, int16_t p)
{
	if(!get_present()) {
		Overheat = 0;
		err = OK;
		return Overheat;
	} // ЭРВ в конфиге нет
	// вычисляется в зависимости от алгоритма
#ifdef DEMO
	//Overheat = 400;
	Overheat = random(100,400);
	int16_t tPEVA = HP.sADC[PEVA].get_present() ? PressToTemp(PEVA, _data.typeFreon) : -32767;
#else
	int16_t tPEVA = HP.sADC[PEVA].get_present() ? PressToTemp(PEVA, _data.typeFreon) : -32767;
	switch(_data.ruleEEV)  // определение доступности элемента
	{
#if defined(TEVAIN) && defined(TEVAOUT)
	case TEVAOUT_TEVAIN:
		/* [xpik_nsk]:
		 * Перегрев = Т2 - (Т1 - Т (дельта P на испарителе), где
		 * Т2 - температура на выходе испарителя,
		 * Т1 - температура на входе испарителя,
		 * Т (дельта Р) - потеря давления на испарителе переведенная в гр, это значение зависит в том числе и от массового расхода, поэтому при изменении производительности компрессора будет также изменяться.
		 */
		if((HP.sTemp[TEVAOUT].get_present())&&(HP.sTemp[TEVAIN].get_present())) {
xTEVAOUT_TEVAIN:
#if defined(TCONIN)
			Overheat = (fHeating ? HP.sTemp[TEVAOUT].get_Temp() - HP.sTemp[TEVAIN].get_Temp() : HP.sTemp[TCONIN].get_Temp() - HP.sTemp[TCONOUT].get_Temp()) + _data.Correction;
#elif defined(TCOMPIN)
			Overheat = (fHeating ? HP.sTemp[TEVAOUT].get_Temp() - HP.sTemp[TEVAIN].get_Temp() : HP.sTemp[TCOMPIN].get_Temp() - HP.sTemp[TCONOUT].get_Temp()) + _data.Correction;
#endif
		} else {
			err = ERR_TYPE_OVERHEAT;
			set_Error(err, name);
		}
		break;
#endif
#if defined(TEVAIN) && defined(TCOMPIN)
	case TCOMPIN_TEVAIN:
		if((HP.sTemp[TCOMPIN].get_present())&&(HP.sTemp[TEVAIN].get_present())) {
xTCOMPIN_TEVAIN:
			Overheat = HP.sTemp[TCOMPIN].get_Temp() - (fHeating ? HP.sTemp[TEVAIN].get_Temp() : HP.sTemp[TCONOUT].get_Temp()) + _data.Correction;
		} else {
			err = ERR_TYPE_OVERHEAT;
			set_Error(err, name);
		}
		break;
#endif
	case TEVAOUT_PEVA:
		if((HP.sTemp[TEVAOUT].get_present()) && tPEVA != -32767) {
xTEVAOUT_PEVA:
#if defined(TCONIN)
			Overheat = (fHeating ? HP.sTemp[TEVAOUT].get_Temp() : HP.sTemp[TCONIN].get_Temp()) - tPEVA + _data.Correction;
#elif defined(TCOMPIN)
			Overheat = (fHeating ? HP.sTemp[TEVAOUT].get_Temp() : HP.sTemp[TCOMPIN].get_Temp()) - tPEVA + _data.Correction;
#endif
		} else {
			err = ERR_TYPE_OVERHEAT;
			set_Error(err, name);
		}
		break;
#ifdef TCOMPIN
	case TCOMPIN_PEVA:
		if(HP.sTemp[TCOMPIN].get_present() && tPEVA != -32767) {
xTCOMPIN_PEVA: Overheat = HP.sTemp[TCOMPIN].get_Temp() - tPEVA + _data.Correction;
		} else {
			err = ERR_TYPE_OVERHEAT;
			set_Error(err, name);
		}
		break;
#endif
#if defined(TEVAIN)
	case TABLE:
#endif
	case MANUAL:
	default:
		if((HP.sTemp[TEVAOUT].get_present()) && tPEVA != -32767) goto xTEVAOUT_PEVA;
		else
#ifdef TCOMPIN
		if((HP.sTemp[TCOMPIN].get_present()) && tPEVA != -32767) goto xTCOMPIN_PEVA;
		else
#endif
#if defined(TCOMPIN) && defined(TEVAIN)
		if((HP.sTemp[TCOMPIN].get_present()) && tPEVA != -32767) goto xTCOMPIN_TEVAIN;
		else
#endif
#if defined(TEVAIN) && defined(TEVAOUT)
		if((HP.sTemp[TEVAOUT].get_present()) && tPEVA != -32767) goto xTEVAOUT_TEVAIN;
		else
#endif
		{
			err = ERR_TYPE_OVERHEAT;
			set_Error(err, name);
		}
		break;
	}
#endif

#ifdef TCOMPIN
	OverheatTCOMP = HP.sTemp[TCOMPIN].get_Temp() - tPEVA + _data.Correction;
#endif
	err = OK;
	// if (Overheat<-100) err=set_Error(ERR_OVERHEAT,name);   // Отрицательный перегрев????? даем запас -1 градуса
	return Overheat;
}

// Сброс пид регулятора
void devEEV::resetPID()
{
	// Инициализировать служебные переменные
#ifdef PID_FORMULA2
	pidw.PropOnMeasure = GETBIT(_data.flags, fPID_PropOnMeasure);
	pidw.min = _data.minSteps * 1000 * 100;
	pidw.max = _data.maxSteps * 1000 * 100;
	pidw.sum = EEV * 1000 * 100;
	pidw.pre_err = _data.tOverheat - Overheat;
#else
	pidw.sum = 0;
	pidw.pre_err = 0;
	pidw.max = EEV_MAX_INT_PID * 1000*100; // максимальное воздействие интегральной составляющей на ПИД
#endif
	if(GETBIT(_data.flags, fEEV_DirectAlgorithm)) {
		pidw.max = 0;
		pidw.sum = 0;
		pidw.pre_err = _data.tOverheat - Overheat;
		pidw.pre_err2[0] = _data.tOverheatTCOMP - OverheatTCOMP;
	}
}

#define sign_dif(a,b) (a > 0 ? 1 : a < 0 ? -1 : -sign(b))

// Обновление ЭРВ - одна итерация алгоритма отслеживания
// на входе две температуры, используется для алгоритма table
int8_t devEEV::Update(void) //boolean fHeating)
{
	if(!GETBIT(_data.flags, fPresent)) return err;  // если ЭРВ нет то ничего не делаем
	if(fPause) return err;    // если пауза то выходим
	int16_t newEEV = 0;
	switch(_data.ruleEEV)     // В зависмости от правила вычисления перегрева
	{
#if defined(TEVAIN)
	case TEVAOUT_TEVAIN:
#endif
#if defined(TCOMPIN)
	case TCOMPIN_TEVAIN:
#endif
	case TEVAOUT_PEVA:
#ifdef TCOMPIN
	case TCOMPIN_PEVA:
#endif
		if(GETBIT(_data.flags, fEEV_DirectAlgorithm)) {
#if defined(TCOMPIN)
			int16_t diff = _data.tOverheatTCOMP - OverheatTCOMP;
			int8_t fast = signm(pidw.pre_err2[0] - diff, _data.trend_mul_threshold); // 0, +-1, +-2
			pidw.trend[trOH_TCOMP] += fast;
			pidw.pre_err2[0] = diff;
			if(pidw.trend[trOH_TCOMP] > _data.trend_threshold * 2) {
				pidw.trend[trOH_TCOMP] = _data.trend_threshold * 2;
				fast--;
			} else if(pidw.trend[trOH_TCOMP] < -_data.trend_threshold * 2) {
				pidw.trend[trOH_TCOMP] = -_data.trend_threshold * 2;
				fast++;
			} else fast = 0;
			diff = _data.tOverheat - Overheat;
			pidw.trend[trOH_default] += signm(pidw.pre_err - diff, _data.trend_mul_threshold); //sign_dif(pidw.pre_err - diff, pidw.trend[trOH_default]); // +1 - растет, -1 - падает
			pidw.pre_err = diff;
			if(pidw.trend[trOH_default] > _data.trend_threshold * 2) {
				pidw.trend[trOH_default] = _data.trend_threshold * 2;
			} else if(pidw.trend[trOH_default] < -_data.trend_threshold * 2) {
				pidw.trend[trOH_default] = -_data.trend_threshold * 2;
			}
#ifdef DEBUG_PID
			journal.printf("EEV: %d=%d,%d=%d. ", _data.tOverheat - Overheat, pidw.trend[trOH_default], _data.tOverheatTCOMP - OverheatTCOMP, pidw.trend[trOH_TCOMP]);
#endif
			if(pidw.max) {
#ifdef DEBUG_PID
				journal.printf("skip:%d\n", pidw.max);
#endif
				pidw.max--;
			} else {
				if(diff < -_data.pid2_delta) { // Перегрев больше, проверка порога - открыть ЭРВ
					if(pidw.trend[trOH_default] >= _data.trend_threshold) {
						newEEV = (int32_t)diff * _data.pid.Kp / (100*1000);
						pidw.max = _data.trend_threshold;
						pidw.trend[trOH_default] = 0;
						pidw.trend[trOH_TCOMP] = 0;
					} else if(pidw.trend[trOH_default] > 0) {
						newEEV = 1;
						pidw.max = 1;
						//if(pidw.trend[trOH_default] > 0) pidw.trend[trOH_default] = 0;
					} else goto xSecond;
				} else if(diff > _data.pid2_delta) { // Перегрев меньше, проверка порога - закрыть ЭРВ
					if(pidw.trend[trOH_default] <= -_data.trend_threshold) {
						newEEV = (int32_t)diff * _data.pid.Kp / (100*1000);
						pidw.max = _data.trend_threshold;
						pidw.trend[trOH_default] = 0;
						pidw.trend[trOH_TCOMP] = 0;
					} else if(pidw.trend[trOH_default] > 0) {
						newEEV = -1;
						pidw.max = 1;
					} else goto xSecond;
				} else {
xSecond:			if(pidw.pre_err2[0] < -_data.tOverheatTCOMP_delta) { // Перегрев больше, проверка порога - открыть ЭРВ
						if(pidw.trend[trOH_TCOMP] > _data.trend_threshold) {
							newEEV = 1;
							pidw.trend[trOH_TCOMP] = 0;
						}
					} else if(pidw.pre_err2[0] > _data.tOverheatTCOMP_delta) {
						if(pidw.pre_err2[0] > _data.tOverheatTCOMP_delta * 2 || OverheatTCOMP <= 0) { // слишком низко
						    if(pidw.trend[trOH_TCOMP] < _data.trend_threshold || OverheatTCOMP <= 0) {
								newEEV = (int32_t)pidw.pre_err2[0] * _data.pid.Kp / (100*1000) / 2;
								if(newEEV == 0) newEEV = -1;
								pidw.max = _data.trend_threshold;
								pidw.trend[trOH_default] = 0;
								pidw.trend[trOH_TCOMP] = 0;
							}
						} else if(pidw.trend[trOH_TCOMP] <= -_data.trend_threshold) {
							newEEV = -1;
							pidw.trend[trOH_TCOMP] = 0;
						}
					} else if(fast) {
						newEEV = fast;
						pidw.trend[trOH_TCOMP] = 0;
					}
				}
#ifdef DEBUG_PID
				journal.printf("%d\n", newEEV);
#endif
			}
			if(newEEV > _data.pid_max) newEEV = _data.pid_max;
			else if(newEEV < -_data.pid_max) newEEV = -_data.pid_max;
			newEEV += EEV;
#endif
		} else {
			newEEV = _data.tOverheat - Overheat;   // Расчет ошибки для пида
#ifdef PID_FORMULA2
			newEEV = round_div_int16(updatePID(newEEV, abs(newEEV) < _data.pid2_delta ? _data.pid2 : _data.pid, pidw), 100); // Рассчитaть итерацию: Перевод в шаги (выход ПИДА в сотых) + округление и добавление предудущего значения
#else  // Алгоритм 1
            pidw.Kp_dmin=_data.pid2_delta; // передать параметр - уменьшение пропорциональной при определенной ошибке
			newEEV = updatePID(newEEV, _data.pid, pidw)/100; // Рассчитaть итерацию: Перевод в шаги (выход ПИДА в сотых) + округление вниз
	//		newEEV = round_div_int16(updatePID(newEEV, _data.pid, pidw), 100); // Рассчитaть итерацию: Перевод в шаги (выход ПИДА в сотых) + округление здесь 0.5 это один шаг
			if ((abs(newEEV)>_data.pid_max)&&(_data.pid_max>0)) {if (newEEV>0) newEEV=EEV+_data.pid_max; else newEEV=EEV-_data.pid_max;} else newEEV+=EEV;   // Ограничение пида +  добавление предудущего значения
#endif
			// Проверка управляющего воздействия, возможно отказ ЭРВ
			//      Serial.print("errPID="); Serial.print(errPID,4);Serial.print(" newEEV=");Serial.print(newEEV);Serial.print(" EEV=");Serial.println(EEV);
		}
		break;
#if defined(TEVAIN)
	case TABLE:
		newEEV = TempToEEV((HP.sTemp[TEVAOUT].get_Temp() + HP.sTemp[TEVAIN].get_Temp()) / 2,
#if defined(TCONIN)
				(HP.sTemp[TCONOUT].get_Temp() + HP.sTemp[TCONIN].get_Temp()) / 2);
#elif defined(TCOMPIN)
				(HP.sTemp[TCONOUT].get_Temp() + HP.sTemp[TCOMPIN].get_Temp()) / 2);
#endif
		break;
#endif
	case MANUAL:
		newEEV = _data.manualStep;
		break;
	default:
		return err;
	}
	if(newEEV < _data.minSteps) {
#if !defined(DEMO) && defined(EEV_MIN_CONTROL) // Контролировать достижение минимального открытия, ошибка генерится
		if(HP.is_compressor_on()) {   // во время работы - Сообщение
			err = _data.minSteps;
			set_Error(err, (char*) name);
			return err;
		}
#endif
		newEEV = _data.minSteps;            // ограничение
	}
	if(newEEV > _data.maxSteps) {
#if !defined(DEMO) && defined(EEV_MAX_CONTROL)   // если задан контроль верхнего диапазона
		if(HP.is_compressor_on()) {   // во время работы - Сообщение
			err = ERR_MAX_EEV;
			set_Error(err, (char*) name);
			return err;
		}
#endif
		newEEV = _data.maxSteps;            // ограничение
	}
	//  Передвинуть шаговик ЭРВ в позицию (абсолютную) EEV если есть изменения
	if(newEEV != EEV) set_EEV(newEEV);
	return err;
}

void devEEV::CorrectOverheat(void)
{
#ifdef DEF_OHCor_OverHeatStart
	static uint16_t OverHeatCor_period = 0; // Только для одного ЭРВ.
	int16_t t = HP.get_temp_condensing();
	int16_t t2 = HP.get_temp_evaporating();
	OHCor_tdelta = (int32_t)_data.OHCor_TDIS_TCON + (t - 3000) * DEF_OHCor_CONDENSING_30_MUL / 1000 - (int32_t)t2 * DEF_OHCor_EVAPORATING_0_MUL / 1000
#ifdef TCOMPIN
					+ (HP.sTemp[TCOMPIN].get_Temp() - t2
#else
					+ (Overheat
#endif
					- DEF_OHCor_OverHeatStart);
	if(fPause || !GETBIT(_data.flags, fCorrectOverHeat)) return;
	if(rtcSAM3X8.unixtime() - HP.get_startCompressor() > _data.OHCor_Delay && ++OverHeatCor_period >= _data.OHCor_Period) {
		OverHeatCor_period = 0;
		t = (HP.sTemp[TCOMP].get_Temp() - t) - OHCor_tdelta;
		if(t > _data.OHCor_TDIS_TCON_Thr) { // Разница большая - уменьшаем перегрев. o = omin + d_curr * (ost - omin) / d_max
			if(OHCor_tdelta_prev <= OHCor_tdelta) { // дельта не изменилась или растет
				t2 = (int32_t) OHCor_tdelta + OHCor_tdelta * _data.OHCor_TDIS_TCON_MAX / 100;
				if(t >= t2) {
					_data.tOverheat = _data.OverHeatStart;
				} else {
					_data.tOverheat = (int32_t) _data.OverheatMin + t * (_data.OverHeatStart - _data.OverheatMin) / t2;
				}
			}
		} else if(t < -_data.OHCor_TDIS_TCON_Thr) { // Разница маленькая - увеличиваем перегрев
			if(_data.tOverheat >= _data.OverHeatStart) _data.tOverheat = _data.OverheatMax;
			else _data.tOverheat = _data.OverHeatStart;
#ifdef DEBUG_MODWORK
			journal.jprintf("OHCor: delta too low: %.2f, set ОН: %.2f\n", (float)OHCor_tdelta / 100, (float)_data.tOverheat / 100.0);
#endif
		} else {
			_data.tOverheat = _data.OverHeatStart;
		}
		OHCor_tdelta_prev = OHCor_tdelta;
//		if(t > _data.OverheatMax) t = _data.OverheatMax;
//		else if(t < _data.OverheatMin) t = _data.OverheatMin;
//		_data.tOverheat = t;
	}
#endif
}

void devEEV::CorrectOverheatInit(void)
{
    if(GETBIT(_data.flags, fCorrectOverHeat)) {    // Установка начального перегрева
	    _data.tOverheat = _data.OverHeatStart;
    }
	OHCor_tdelta = 0;
}

void devEEV::after_load(void)
{
	if(HP.Option.ver == 128) { // Конвертация флагов
		_data.flags = _data.OHCor_TDIS_TCON_MAX;
		_data.OHCor_TDIS_TCON_MAX = 50;
	}
#ifdef EEV_DEF
	SETBIT1(_data.flags,fPresent);                      // наличие ЭРВ в текушей конфигурации
#else
	SETBIT0(_data.flags,fPresent);                      // отсутствие ЭРВ в текушей конфигурации
#endif
}

 // Получить параметр ЭРВ в виде строки
 // var - строка с параметром ret-выходная строка, ответ ДОБАВЛЯЕТСЯ
char* devEEV::get_paramEEV(char *var, char *ret)
{
	if(strcmp(var, eev_POS)==0) {
		_itoa(EEV,ret);
	} else if(strcmp(var, eev_POSp)==0){
		_ftoa(ret,(float)get_EEV_percent() / 100, 1);
	} else if(strcmp(var, eev_POSpp)==0){
		_itoa(EEV,ret);
		strcat(ret," (");
		_ftoa(ret, (float)get_EEV_percent() / 100, 1);
		strcat(ret,"%)");
		if(stepperEEV.isBuzy())  strcat(ret,"⇔");  // признак движения
	} else if(strcmp(var, eev_OVERHEAT)==0){
		_ftoa(ret,(float)Overheat/100,2);
	} else if(strcmp(var, eev_ERROR)==0){  _itoa(err,ret);
	} else if(strcmp(var, eev_MIN)==0){    _itoa(_data.minSteps,ret);
	} else if(strcmp(var, eev_MAX)==0){	   _itoa(_data.maxSteps,ret);
	} else if(strcmp(var, eev_TIME)==0){   _itoa(_data.pid_time,ret);
	} else if(strcmp(var, eev_TARGET)==0){ _ftoa(ret,(float)_data.tOverheat/100,2);
	} else if(strcmp(var, eev_tOverheatTCOMP)==0){ _ftoa(ret,(float)_data.tOverheatTCOMP/100,2);
	} else if(strcmp(var, eev_tOverheatTCOMP_delta)==0){ _ftoa(ret,(float)_data.tOverheatTCOMP_delta/100,2);
	} else if(strcmp(var, eev_PID2_delta)==0){ _ftoa(ret, (float)_data.pid2_delta/100, 2);
	} else if(strcmp(var, eev_KP)==0){     _ftoa(ret,(float)-_data.pid.Kp / 1000,3);
	} else if(strcmp(var, eev_KP2)==0){    _ftoa(ret,(float)-_data.pid2.Kp / 1000,3);
	} else if(strcmp(var, eev_KI2)==0){	   _ftoa(ret,(float)-_data.pid2.Ki / _data.pid_time / 1000,3);
	} else if(strcmp(var, eev_KD2)==0){	   _ftoa(ret,(float)-_data.pid2.Kd * _data.pid_time / 1000,3);
#ifdef PID_FORMULA2
	} else if(strcmp(var, eev_KI)==0){	   _ftoa(ret,(float)-_data.pid.Ki / _data.pid_time / 1000,3);
	} else if(strcmp(var, eev_KD)==0){	   _ftoa(ret,(float)-_data.pid.Kd * _data.pid_time / 1000,3);
#else
	} else if(strcmp(var, eev_KI)==0){	   _ftoa(ret,(float)-_data.pid.Ki / 1000,3);
	} else if(strcmp(var, eev_KD)==0){	   _ftoa(ret,(float)-_data.pid.Kd / 1000,3);
	} else if(strcmp(var, eev_ERR_KP)==0){ 	_ftoa(ret, (float)_data.pid2_delta/100, 2);
#endif
	} else if(strcmp(var, eev_CONST)==0){  _ftoa(ret,(float)_data.Correction/100,2);
	} else if(strcmp(var, eev_MANUAL)==0){ _itoa(_data.manualStep,ret);
	} else if(strcmp(var, eev_FREON)==0){
		for(uint8_t i=0;i<=R717;i++) // Формирование списка фреонов
		{ strcat(ret,noteFreon[i]); strcat(ret,":"); if(i==get_typeFreon()) strcat(ret,cOne); else strcat(ret,cZero); strcat(ret,";");  }
	}   else if(strcmp(var, eev_RULE)==0){
		web_fill_tag_select(ret, noteRuleEEV, get_ruleEEV());
	} else if(strcmp(var, eev_NAME)==0){    strcat(ret,name);
	} else if(strcmp(var, eev_NOTE)==0){    strcat(ret,note);
	} else if(strcmp(var, eev_REMARK)==0){  strcat(ret,noteRemarkEEV[get_ruleEEV()]);
	} else if(strcmp(var, eev_PINS)==0){
		strcat(ret,"D");  _itoa(PIN_EEV1_D24,ret);
		strcat(ret," D"); _itoa(PIN_EEV2_D25,ret);
		strcat(ret," D"); _itoa(PIN_EEV3_D26,ret);
		strcat(ret," D"); _itoa(PIN_EEV4_D27,ret);
	} else if(strcmp(var, eev_cCORRECT)==0){_itoa((_data.flags & (1<<fCorrectOverHeat)) != 0, ret);
	} else if(strcmp(var, eev_cDELAY)==0){ 	_itoa(_data.OHCor_Delay, ret);
	} else if(strcmp(var, eev_cPERIOD)==0){	_itoa(_data.OHCor_Period, ret);
	} else if(strcmp(var, eev_cDELTA)==0){ 	_ftoa(ret, (float)_data.OHCor_TDIS_TCON/100, 2);
	} else if(strcmp(var, eev_cOH_MIN)==0){	_ftoa(ret, (float)_data.OverheatMin/100, 2);
	} else if(strcmp(var, eev_cOH_MAX)==0){	_ftoa(ret, (float)_data.OverheatMax/100, 2);
	} else if(strcmp(var, eev_cOH_START)==0){_ftoa(ret, (float)_data.OverHeatStart/100, 2);
	} else if(strcmp(var, eev_cOH_TDELTA)==0){if(OHCor_tdelta) _ftoa(ret, (float)OHCor_tdelta/100, 2); else strcat(ret, "-");
	} else if(strcmp(var, eev_cOH_START)==0){_ftoa(ret, (float)_data.OverHeatStart/100, 2);
    } else if(strcmp(var, eev_cDELTA_Thr)==0){	_ftoa(ret, (float)(_data.OHCor_TDIS_TCON_Thr/100), 1);
    } else if(strcmp(var, eev_cOH_cDELTA_MAX)==0){	_itoa(_data.OHCor_TDIS_TCON_MAX, ret);
    } else if(strcmp(var, eev_PID_MAX)==0){	_itoa(_data.pid_max, ret); // ограничение ПИД в шагах ЭРВ
	} else if(strcmp(var, eev_SPEED)==0){  	_itoa(_data.speedEEV, ret);
	} else if(strcmp(var, eev_PRE_START_POS)==0){	_itoa(_data.preStartPos, ret);
	} else if(strcmp(var, eev_START_POS)==0){    	_itoa(_data.StartPos, ret);
	} else if(strcmp(var, eev_PosAtHighTemp)==0){ 	_itoa(_data.PosAtHighTemp, ret);
	} else if(strcmp(var, eev_DELAY_ON_PID)==0){  	_itoa(_data.delayOnPid, ret);
	} else if(strcmp(var, eev_DELAY_START_POS)==0){	_itoa(_data.DelayStartPos, ret);
	} else if(strcmp(var, eev_DELAY_OFF)==0){	  	_itoa(_data.delayOff, ret);
	} else if(strcmp(var, eev_DELAY_ON)==0){	   	_itoa(_data.delayOn, ret);
	} else if(strcmp(var, eev_HOLD_MOTOR)==0){   	_itoa((_data.flags & (1<<fHoldMotor))!=0, ret);
	} else if(strcmp(var, eev_PRESENT)==0){         _itoa((_data.flags & (1<<fPresent))!=0, ret);
	} else if(strcmp(var, eev_SEEK_ZERO)==0){   	_itoa((_data.flags & (1<<fOneSeekZero))!=0, ret);
	} else if(strcmp(var, eev_CLOSE)==0){           _itoa((_data.flags & (1<<fEevClose))!=0, ret);
	} else if(strcmp(var, eev_LIGHT_START)==0){    	_itoa((_data.flags & (1<<fLightStart))!=0, ret);
	} else if(strcmp(var, eev_START )==0){          _itoa((_data.flags & (1<<fStartFlagPos))!=0, ret);
	} else if(strcmp(var, eev_PID_P_ON_M )==0){     _itoa((_data.flags & (1<<fPID_PropOnMeasure))!=0, ret);
	} else if(strcmp(var, eev_fEEVStartPosByTemp)==0){ _itoa((_data.flags & (1<<fEEV_StartPosByTemp))!=0, ret);
	} else if(strcmp(var, eev_fEEV_DirectAlgorithm)==0){ _itoa((_data.flags & (1<<fEEV_DirectAlgorithm))!=0, ret);
	} else if(strcmp(var, eev_trend_threshold)==0){	_itoa(_data.trend_threshold, ret);
	} else if(strcmp(var, eev_trend_mul_threshold)==0){	_ftoa(ret, (float)_data.trend_mul_threshold/100, 2);
	} else strcat(ret,"E10");
	return ret;
}

// Установить параметр ЭРВ из флоат параметр var
// в случае успеха возврщает true
boolean devEEV::set_paramEEV(char *var,float x)
{
	if(strcmp(var, eev_POS)==0) {
		if ((x>=_data.minSteps)&&(x<=_data.maxSteps)){ set_EEV(x); return true;} else return false;
	} else if(strcmp(var, eev_POSp)==0){
		x = x * _data.maxSteps / 100;
		if ((x >= _data.minSteps)&&(x <= _data.maxSteps)) { set_EEV(x); return true;} else return false;
	} else if(strcmp(var, eev_POSpp)==0){
		return true;  // не имеет смысла - только чтение
	} else if(strcmp(var, eev_MIN)==0){
		if ((x>=0)&&(x<_data.maxSteps)) { _data.minSteps=x; return true;} else return false;	// минимальное число шагов
		return true;
	} else if(strcmp(var, eev_MAX)==0){
		if ((x>=_data.minSteps)&&(x<2000)) { _data.maxSteps=x; return true;} else return false;	// максимальное число шагов
		return true;
	} else if(strcmp(var, eev_TIME)==0){
		if((x >= 1) && (x <= 1000)) {
			if(_data.pid_time != x) {
				UpdatePIDbyTime(x, _data.pid_time, _data.pid);
				UpdatePIDbyTime(x, _data.pid_time, _data.pid2);
				_data.pid_time = x;
			}
			return true;
		} else return false;	// секунды
	} else if(strcmp(var, eev_TARGET)==0){ 
		if ((x>0.0)&&(x<=50.0)) { _data.tOverheat=rd(x, 100); ;return true;}  else return false;	// цель сотые градуса
	} else if(strcmp(var, eev_tOverheatTCOMP)==0){
		_data.tOverheatTCOMP = rd(x, 100); return true;
	} else if(strcmp(var, eev_tOverheatTCOMP_delta)==0){
		_data.tOverheatTCOMP_delta = rd(x, 100); return true;
	} else if(strcmp(var, eev_KP)==0){
		_data.pid.Kp = -rd(x, 1000); return true;
	} else if(strcmp(var, eev_KI)==0){
#ifdef PID_FORMULA2
		_data.pid.Ki = -rd(x * _data.pid_time, 1000);
#else
		_data.pid.Ki = -rd(x, 1000);
#endif
		return true;
	} else if(strcmp(var, eev_KD) == 0) {
#ifdef PID_FORMULA2
		_data.pid.Kd = -rd(x / _data.pid_time, 1000);
#else
		_data.pid.Kd = -rd(x, 1000);
#endif
		return true;
	} else if(strcmp(var, eev_KP2)==0){
		_data.pid2.Kp = -rd(x, 1000); return true;
	} else if(strcmp(var, eev_KI2)==0){
		_data.pid2.Ki = -rd(x * _data.pid_time, 1000);
		return true;
	} else if(strcmp(var, eev_KD2) == 0) {
		_data.pid2.Kd = -rd(x / _data.pid_time, 1000);
		return true;
	} else if(strcmp(var, eev_CONST)==0){
		if ((x>=-10.0)&&(x<=10.0)) { _data.Correction=rd(x, 100); return true;}else return false;	// сотые градуса
	} else if(strcmp(var, eev_MANUAL)==0){
		if ((x>=_data.minSteps)&&(x<=_data.maxSteps)){ _data.manualStep = x; if(_data.ruleEEV == MANUAL) set_EEV(_data.manualStep); return true; } else return false;	// шаги
	} else if(strcmp(var, eev_FREON)==0){
		if ((x>=0)&&(x<=R717)){ _data.typeFreon=(TYPEFREON)x; return true;} else return false;	// перечисляемый тип
	}   else if(strcmp(var, eev_RULE)==0){
		if(x <= MANUAL) {
			_data.ruleEEV = (RULE_EEV) x;
			resetPID();
			return true;
		} else return false;	// перечисляемый тип
	} else if(strcmp(var, eev_cCORRECT)==0){
		if (x==0) SETBIT0(_data.flags, fCorrectOverHeat); else SETBIT1(_data.flags, fCorrectOverHeat);
	} else if(strcmp(var, eev_cDELAY)==0){
		if ((x>=0)&&(x<=10000)) { _data.OHCor_Delay=x; return true;} else return false;	// секунды
	} else if(strcmp(var, eev_cPERIOD)==0){
		if ((x>=0)&&(x<=10000)) { _data.OHCor_Period=x; return true;} else return false;	// циклы ЭРВ
	} else if(strcmp(var, eev_cDELTA)==0){
		if ((x>=-10.0)&&(x<=50.0)) {_data.OHCor_TDIS_TCON=rd(x, 100); return true;}else return false;	// сотые градуса
	} else if(strcmp(var, eev_cDELTA_Thr)==0){
		_data.OHCor_TDIS_TCON_Thr = rd(x, 100); return true; // сотые градуса
	} else if(strcmp(var, eev_cOH_cDELTA_MAX)==0){
		_data.OHCor_TDIS_TCON_MAX = x; return true;
	} else if(strcmp(var, eev_PID_MAX)==0){ // ограничение ПИД в шагах ЭРВ
		if ((x>=0.0)&&(x<=200.0)) {_data.pid_max = x; return true;} else return false;
	} else if(strcmp(var, eev_cOH_MIN)==0){
		if ((x>=0.0)&&(x<=50.0)) {_data.OverheatMin=rd(x, 100); return true;}else return false;	// сотые градуса
	} else if(strcmp(var, eev_cOH_MAX)==0){
		if ((x>=0.0)&&(x<=50.0)) {_data.OverheatMax=rd(x, 100); return true;}else return false;	// сотые градуса
	} else if(strcmp(var, eev_cOH_START)==0){
		if ((x>=0.0)&&(x<=50.0)) {_data.OverHeatStart=rd(x, 100); return true;}else return false;	// сотые градуса
	} else if(strcmp(var, eev_PID2_delta)==0){
		_data.pid2_delta=rd(x, 100); return true; // сотые
	} else if(strcmp(var, eev_SPEED)==0){
		if ((x>=5)&&(x<=120)) { _data.speedEEV=x; return true;} else return false;	// шаги в секунду
	} else if(strcmp(var, eev_PRE_START_POS)==0){
		if ((x>=_data.minSteps)&&(x<=_data.maxSteps)) { _data.preStartPos=x; return true;} else return false;	// шаги
	} else if(strcmp(var, eev_START_POS)==0){
		if ((x>=_data.minSteps)&&(x<=_data.maxSteps)) { _data.StartPos=x; return true;} else return false;	// шаги
	} else if(strcmp(var, eev_PosAtHighTemp)==0){
		_data.PosAtHighTemp=x; return true;	// шаги
	} else if(strcmp(var, eev_DELAY_ON_PID)==0){
		if ((x>=0)&&(x<=255)) { _data.delayOnPid=x; return true;} else return false;	// секунды размер 1 байт
	} else if(strcmp(var, eev_DELAY_START_POS)==0){
		if ((x>=0)&&(x<=255)) { _data.DelayStartPos=x; return true;} else return false;	// секунды размер 1 байт
	} else if(strcmp(var, eev_DELAY_OFF)==0){
		if ((x>=0)&&(x<=255)) { _data.delayOff=x; return true;} else return false;	// секунды размер 1 байт
	} else if(strcmp(var, eev_DELAY_ON)==0){
		if ((x>=0)&&(x<=255)) { _data.delayOn=x; return true;} else return false;	// секунды размер 1 байт
	} else if(strcmp(var, eev_HOLD_MOTOR)==0){
		if (x==0) SETBIT0(_data.flags, fHoldMotor); else SETBIT1(_data.flags, fHoldMotor);
	} else if(strcmp(var, eev_SEEK_ZERO)==0){
		if (x==0) SETBIT0(_data.flags, fOneSeekZero); else SETBIT1(_data.flags, fOneSeekZero);
	} else if(strcmp(var, eev_CLOSE)==0){
		if (x==0) SETBIT0(_data.flags, fEevClose); else SETBIT1(_data.flags, fEevClose);
	} else if(strcmp(var, eev_LIGHT_START)==0){
		if (x==0) SETBIT0(_data.flags, fLightStart); else SETBIT1(_data.flags, fLightStart);
	} else if(strcmp(var, eev_START)==0){
		if (x==0) SETBIT0(_data.flags, fStartFlagPos); else SETBIT1(_data.flags, fStartFlagPos);
	} else if(strcmp(var, eev_fEEVStartPosByTemp)==0){
		if (x==0) SETBIT0(_data.flags, fEEV_StartPosByTemp); else SETBIT1(_data.flags, fEEV_StartPosByTemp);
	} else if(strcmp(var, eev_fEEV_DirectAlgorithm)==0){
		if (x==0) SETBIT0(_data.flags, fEEV_DirectAlgorithm); else SETBIT1(_data.flags, fEEV_DirectAlgorithm);
		resetPID();
	} else if(strcmp(var, eev_PID_P_ON_M)==0){
		if (x==0) SETBIT0(_data.flags, fPID_PropOnMeasure); else SETBIT1(_data.flags, fPID_PropOnMeasure);
		resetPID();
	} else if(strcmp(var, eev_trend_threshold)==0){	_data.trend_threshold = x; return true;
	} else if(strcmp(var, eev_trend_mul_threshold)==0){	_data.trend_mul_threshold = rd(x, 100); return true;
	} else return false; // ошибочное имя параметра

	return true;  // для флагов
}

void devEEV::get_ruleEEVtext(char *strReturn)
{
	switch((int)HP.dEEV.get_ruleEEV()) {
	case TEVAOUT_PEVA:
		strcat(strReturn, "TEVAOUT-PEVA");
		break;
#ifdef TCOMPIN
	case TCOMPIN_PEVA:
		strcat(strReturn, "TCOMPIN-PEVA");
		break;
#endif
#ifdef TEVAIN
	case TEVAOUT_TEVAIN:
		strcat(strReturn, "TEVAOUT-TEVAIN");
		break;
	case TCOMPIN_TEVAIN:
		strcat(strReturn, "TCOMPIN-TEVAIN");
		break;
	case TABLE:
		strcat(strReturn, "TABLE");
		break;
#endif
	case MANUAL:
		strcat(strReturn, "MANUAL");
		break;
	default:
		strcat(strReturn, "*ERROR*");
		break;
	}
}

#endif

#ifndef FC_VACON
// ------------------------------------------------------------------------------------------
// ЧАСТОТНЫЙ ПРЕОБРАЗОВАТЕЛЬ ТОЛЬКО ОДНА ШТУКА ВСЕГДА (не массив) ---------------------------
//#define IS_ANALOG       (GETBIT(flags,fAnalog)?true:false)                       // Проверка на требуемый тип управления  если аналоговый то выдает TRUE
                                                   
int8_t devOmronMX2::initFC()
{ 

  err=OK;                           // ошибка частотника (работа) при ошибке останов ТН
  numErr=0;                         // число ошибок чтение по модбасу для статистики
  number_err=0;                     // Число ошибок связи при превышении FC_NUM_READ блокировка инвертора
  FC=0;                             // Целевая частота частотика
  freqFC=0;                         // текущая частота инвертора
  power=0;                          // Тееущая мощность частотника
  current=0;                        // Текуший ток частотника
  startCompressor=0;                // время старта компрессора
  state=ERR_LINK_FC;                // Состояние - нет связи с частотником
  dac=0;                            // Текущее значение ЦАП
  testMode=NORMAL;                                 // Значение режима тестирования
  name=(char*)nameOmron;                           // Имя
  note=(char*)noteFC_NONE;                         // Описание инвертора   типа нет его
  
  _data.Uptime=DEF_FC_UPTIME;				       // Время обновления алгоритма пид регулятора (мсек) Основной цикл управления
  _data.PidFreqStep=DEF_FC_PID_FREQ_STEP;          // Максимальный шаг (на увеличение) изменения частоты при ПИД регулировании в 0.01 Гц Необходимо что бы ЭРВ успевал
  _data.PidStop=DEF_FC_PID_STOP;				   // Проценты от уровня защит (мощность, ток, давление, темпеартура) при которой происходит блокировка роста частоты пидом
  _data.dtCompTemp=DEF_FC_DT_COMP_TEMP;    		   // Защита по температуре компрессора - сколько градусов не доходит до максимальной (TCOMP) и при этом происходит уменьшение частоты
  _data.startFreq=DEF_FC_START_FREQ;               // Стартовая скорость инвертора (см компрессор) в 0.01
  _data.startFreqBoiler=DEF_FC_START_FREQ_BOILER;  // Стартовая скорость инвертора (см компрессор) в 0.01 ГВС
  _data.minFreq=DEF_FC_MIN_FREQ;                   // Минимальная  скорость инвертора (см компрессор) в 0.01
  _data.minFreqCool=DEF_FC_MIN_FREQ_COOL;          // Минимальная  скорость инвертора при охлаждении в 0.01
  _data.minFreqBoiler=DEF_FC_MIN_FREQ_BOILER;      // Минимальная  скорость инвертора при нагреве ГВС в 0.01
  _data.minFreqUser=DEF_FC_MIN_FREQ_USER;          // Минимальная  скорость инвертора РУЧНОЙ РЕЖИМ (см компрессор) в 0.01
  _data.maxFreq=DEF_FC_MAX_FREQ;                   // Максимальная скорость инвертора (см компрессор) в 0.01
  _data.maxFreqCool=DEF_FC_MAX_FREQ_COOL;          // Максимальная скорость инвертора в режиме охлаждения  в 0.01
  _data.maxFreqBoiler=DEF_FC_MAX_FREQ_BOILER;      // Максимальная скорость инвертора в режиме ГВС в 0.01 Гц поглощение бойлера обычно меньше чем СО
  _data.maxFreqUser=DEF_FC_MAX_FREQ_USER;          // Максимальная скорость инвертора РУЧНОЙ РЕЖИМ (см компрессор) в 0.01
  _data.stepFreq=DEF_FC_STEP_FREQ ;                // Шаг уменьшения инвертора при достижении максимальной температуры, мощности и тока (см компрессор) в 0.01
  _data.stepFreqBoiler=DEF_FC_STEP_FREQ_BOILER;    // Шаг уменьшения инвертора при достижении максимальной температуры, мощности и тока ГВС в 0.01
  _data.dtTemp=DEF_FC_DT_TEMP;                     // Привышение температуры от уставок (подача) при которой срабатыват защита (уменьшается частота) в сотых градуса
  _data.dtTempBoiler=DEF_FC_DT_TEMP_BOILER;        // Привышение температуры от уставок (подача) при которой срабатыват защита ГВС в сотых градуса
 #ifdef FC_ANALOG_CONTROL
  _data.level0=0;                                  // Отсчеты ЦАП соответсвующие 0   мощности
  _data.level100=4096;                             // Отсчеты ЦАП соответсвующие 100 мощности
  _data.levelOff=10;                               // Минимальная мощность при котором частотник отключается (ограничение минимальной мощности)
  #endif
  flags=0x00;                               		 // флаги  0 - наличие FC
  _data.setup_flags=0x00;                                // флаги
  if(!Modbus.get_present())                        // modbus отсутствует
      {
       SETBIT0(flags,fFC);          // Инвертор не рабоатет
       journal.jprintf("%s, modbus not found, block.\n",name); 
       err=ERR_NO_MODBUS;
       return err; 
      }
  else if (DEVICEFC==true) SETBIT1(flags,fFC); // наличие частотника в текушей конфигурации
               
  if(get_present())  journal.jprintf("Invertor %s: present config\r\n",name); 
  else {journal.jprintf("Invertor %s: none config\r\n",name);return err;  }  // выходим если нет инвертора

  note=(char*)noteFC_OK;             // Описание инвертора есть
  ChartFC.init(get_present());               // инициалазация графика
  #ifdef FC_ANALOG_CONTROL                      // Аналоговое управление графики не нужны
  	  pin = PIN_DEVICE_FC;                // Ножка куда прицеплено FC
  	  analogWriteResolution(12);        // разрешение ЦАП 12 бит;
  	  analogWrite(pin,dac);
  	  ChartPower.init(false);                 // инициалазация графика
#ifndef MIN_RAM_CHARTS
  	  ChartCurrent.init(false);               // инициалазация графика
#endif
  #else									// НЕ Аналоговое управление
      ChartPower.init(get_present());            // инициалазация графика
#ifndef MIN_RAM_CHARTS
      ChartCurrent.init(get_present());          // инициалазация графика
#endif
      err=Modbus.LinkTestOmronMX2();     // проверка связи с инвертором  xModbusSemaphore не используем так как в один поток
      check_blockFC();   
      if (err!=OK)  return err;          // связи нет выходим
      journal.jprintf("Test link Modbus %s: OK\r\n",name);   // Тест пройден

      // Если частотник работает то остановить его
      get_readState(); //  Получить состояние частотника
      switch (state)   // В зависимости от состояния
      {
        case 0:
        case 2: break;                                                         // ОСТАНОВКА ничего не делаем
        case 3: stop_FC();                                                      // ВРАЩЕНИЕ Послать команду стоп и ждать остановки
                while ((state!=2)||(state!=4)) { get_readState();journal.jprintf("Wait stop %s . . .\r\n",name); _delay(3000); } 
                break;
        case 4:                                                                // ОСТАНОВКА С ВЫБЕГОМ  ждать остановки
                break;
        case 5:stop_FC();                                                      // ТОЛЧОВЫЙ ХОД Послать команду стоп и ждать остановки
                while ((state!=2)||(state!=4)) { get_readState();journal.jprintf("Wait stop %s . . .\r\n",name); _delay(3000); } 
                break;
        case 6:                                                                // ТОРМОЖЕНИЕ ПОСТОЯННЫМ ТОКОМ
        case 7: err=ERR_MODBUS_STATE;set_Error(err,name);  break;             // ВЫПОЛНЕНИЕ ПОВТОРНОЙ ПОПЫТКИ Подъем ошибки на верх и останов ТН
        case 8: break;                                                         // АВАРИЙНОЕ ОТКЛЮЧЕНИЕ
        case 9: break;                                                         // ПОНИЖЕНОЕ ПИТАНИЕ
        case -1:break;
        default:err=ERR_MODBUS_STATE;set_Error(err,name);  break;             // Подъем ошибки на верх и останов ТН
      }
      if (err!=OK) return err;

       // Программирование инвертора Запись различных переменных
      # ifdef  FC_FULL_INIT            // Полная инициализация инвертора
         journal.jprintf(" Full init %s . . .\r\n",name);
         // 1. Источник команды  A001=03  по умолчанию 01
        if ((err=write_0x06_16(MX2_SOURCE_FR, 0x03))==OK)           journal.jprintf(" Setting frequency source (A001) %s: 0x03\r\n",name);
        else                                                        journal.jprintf(" Error setting frequency source (A001) %s code %d\r\n",name,err);
        // 2. Источник задания частоты A002=03  по умолчанию 01
        if ((err=write_0x06_16(MX2_SOURCE_CMD, 0x03))==OK)          journal.jprintf(" Setting run command source (A002) %s: 0x03\r\n",name);
        else                                                        journal.jprintf(" Error setting run command source (A002) %s code %d\r\n",name,err);
        // 3. A003=60  основная частота
        if ((err=write_0x06_16(MX2_BASE_FR, FC_BASE_FREQ/10))==OK) journal.jprintf(" Setting base frequency (A003) %s: %.2f [Hz]\r\n",name,FC_BASE_FREQ);
        else                                                        journal.jprintf(" Error setting base frequency (A003) %s code %d\r\n",name,err);
         // 4. установка максимальной частоты
        if ((err=write_0x06_16(MX2_MAX_FR, maxFreq/10))==OK)   journal.jprintf(" Setting maximum frequency (A004) %s: %.2f [Hz]\r\n",name,maxFreq/100.0);
        else                                                        journal.jprintf(" Error setting maximum frequency (A004) %s code %d\r\n",name,err);
        // 5. Время разгона
        if( write_0x10_32(MX2_ACCEL_TIME,FC_ACCEL_TIME)==OK)          journal.jprintf(" Setting acceleration time (F002) %s: %.2f [sec]\r\n",name,FC_ACCEL_TIME/100.0);
        else                                                        journal.jprintf(" Error setting acceleration time (F002) %s code %d\r\n",name,err);
        // 6. Торможения разгона
        if( write_0x10_32(MX2_DEACCEL_TIME,FC_DEACCEL_TIME)==OK)        journal.jprintf(" Setting deacceleration time (F003) %s: %.2f [sec]\r\n",name,FC_DEACCEL_TIME/100.0);
        else                                                        journal.jprintf(" Error setting deacceleration time (F003) %s code %d\r\n",name,err);
        // 7.  Разрешение торможения постоянным током A051=0
      //  if ((err=write_0x06_16(MX2_DC_BRAKING, 0x01))==OK)          journal.jprintf(" Setting DC braking enable (A051) %s: 0x01\r\n",name);
      //  else                                                        journal.jprintf(" Error setting DC braking enable (A051) %s code %d\r\n",name,err); 
        // 8. Выбор режима ПЧ b171=03
      //  if ((err=write_0x06_16(MX2_MODE, 0x03))==OK)                journal.jprintf(" Setting inverter mode selection (b171) %s: 0x03\r\n",name);
      //  else                                                        journal.jprintf(" Error setting inverter mode selection (b171) %s code %d\r\n",name,err); 
        // 9. B091=01 Выбор способа остановки
        if ((err=write_0x06_16(MX2_STOP_MODE, 0x01))==OK)           journal.jprintf(" Setting stop mode selection (b091) %s: 0x01\r\n",name);
        else                                                        journal.jprintf(" Error setting stop mode selection (b091) %s code %d\r\n",name,err);   
      #endif

#endif  // #ifndef FC_ANALOG_CONTROL
  // 10.Установить стартовую частоту
  set_target(_data.startFreq,true,_data.minFreqUser ,_data.maxFreqUser);       // режим н знаем по этому границы развигаем
  return err;                       
}

// Установить целевую частоту
// параметр показывать сообщение сообщение или нет, два оставшихся параметра границы
int8_t  devOmronMX2::set_target(int16_t x,boolean show, int16_t _min, int16_t _max)
{ 
  err=OK;
  #ifdef DEMO
    if ((x>=_min)&&(x<=_max))                     // Проверка диапазона разрешенных частот
    {FC=x; if(show) journal.jprintf(" Set %s: %.2f [Hz]\r\n",name,FC/100.0);   return err;} // установка частоты OK  - вывод сообщения если надо
     else { journal.jprintf("%s: Wrong frequency %.2f\n",name,x/100.0); return WARNING_VALUE; } 
  #else   // Боевой вариант
  uint16_t hWord,lWord;
  uint8_t i;
  if ((!get_present())||(GETBIT(flags,fErrFC))) return err;    // выходим если нет инвертора или он заблокирован по ошибке
  if ((x>=_min)&&(x<=_max))                     // Проверка диапазона разрешенных частот
   {
  #ifndef FC_ANALOG_CONTROL                                    // Не аналоговое управление
          // Запись в регистры инвертора установленной частоты
          for(i=0;i<FC_NUM_READ;i++)  // Делаем FC_NUM_READ попыток
            {
              err=write_0x10_32(MX2_TARGET_FR,x);
              if (err==OK) break;                     // Команда выполнена
              _delay(100);
              journal.jprintf("%s: repeat set frequency\n",name);  // Выводим сообщение о повторной команде
            }
            
          if(err==OK)  { FC=x; if(show) journal.jprintf(" Set %s: %.2f [Hz]\r\n",name,FC/100.0);return err;} // установка частоты OK  - вывод сообщения если надо
          else {state=ERR_LINK_FC; SETBIT1(flags,fErrFC); set_Error(err,name);return err;}                 // генерация ошибки
   #else  // Аналоговое управление
         FC=x;
         dac=((level100-level0)*FC-0*level100)/(100-0);
         switch (testMode)  // РЕАЛЬНЫЕ Действия в зависимости от режима
             {
              case NORMAL:     analogWrite(pin,dac);  break; //  Режим работа не тест, все включаем
              case SAFE_TEST:                         break; //  Ничего не включаем
              case TEST:                              break; //  Включаем все кроме компрессора
              case HARD_TEST:  analogWrite(pin,dac);  break; //  Все включаем и компрессор тоже
             }
          if(show) journal.jprintf(" Set %s: %.2f [Hz]\r\n",name,FC/100.0); // установка частоты OK  - вывод сообщения если надо
   #endif 
    return err;
   }  // if ((x>=_min)&&(x<=_max)) 
   else { journal.jprintf("%s: Wrong frequency %.2f\n",name,x/100.0); return WARNING_VALUE; }  
  #endif  // DEMO
}

// Установить Отсчеты ЦАП соответсвующие 0   мощности
int8_t devOmronMX2::set_level0(int16_t x)
{
   if ((x>=0)&&(x<=4096)) { level0=x; return OK;} // Только правильные значения
   return WARNING_VALUE;
}
// Установить Отсчеты ЦАП соответсвующие 100 мощности
int8_t devOmronMX2::set_level100(int16_t x)
{
  if ((x>=0)&&(x<=4096)) { level100=x; return OK;} // Только правильные значения
  return WARNING_VALUE;
}
// Установить Минимальная мощность при котором частотник отключается (ограничение минимальной мощности)
int8_t devOmronMX2::set_levelOff(int16_t x)
{
  if ((x>=0)&&(x<=100))  { levelOff=x;  return OK;} // Только правильные значения
  return WARNING_VALUE;
}

// Установить запрет на использование инвертора если лимит ошибок исчерпан
void  devOmronMX2::check_blockFC() 
{
   #ifndef FC_ANALOG_CONTROL                                    // Не аналоговое управление
    if((xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED)&&(err!=OK))   // если не запущена free rtos то блокируем с первого раза
       {
        SETBIT1(flags,fErrFC);                                                  // Установить флаг
        note=(char*)noteFC_NO;
        set_Error(err,(char*)name);                                        // Подъем ошибки на верх и останов ТН
        return; 
       }
        
    if (err!=OK) number_err++;else { number_err=0; return;}       // Увеличить счетчик ошибок
    if (number_err>FC_NUM_READ)                                // если привышено число ошибок то блокировка
      {
       SemaphoreGive(xModbusSemaphore); // разблокировать семафор
       SETBIT1(flags,fErrFC);                                         // Установить флаг
       note=(char*)noteFC_NO;
       set_Error(err,(char*)name);                        // Подъем ошибки на верх и останов ТН
      }
    #endif 
} 

// Прочитать (внутренние переменные обновляются) состояние Инвертора, возвращает или ОК или ошибку
// Вызывается из задачи чтения датчиков период FC_TIME_READ
int8_t devOmronMX2::get_readState()
{
uint8_t i;
if ((!get_present())||(GETBIT(flags,fErrFC))) return err;  // выходим если нет инвертора или он заблокирован по ошибке
err=OK;
#ifndef FC_ANALOG_CONTROL                                    // Не аналоговое управление
  // Чтение состояния инвертора, при ошибке генерация общей ошибки ТН и останов
  for(i=0;i<FC_NUM_READ;i++)   // делаем FC_NUM_READ попыток чтения
      { 
        state=read_0x03_16(MX2_STATE);                     // прочитать состояние
        err=Modbus.get_err();                              // Скопировать ошибку
        if (err==OK)                                       // Прочитано верно
         {
          if ((GETBIT(flags,fOnOff))&&(state!=3)) continue; else break;  // ТН включил компрессор а инвертор не имеет правильного состяния пытаемся прочитать еще один раз в проитвном случае все ок выходим
         } 
        _delay(FC_DELAY_REPEAT); 
        journal.jprintf(cErrorRS485,name,__FUNCTION__,err);// Выводим сообщение о повторном чтении
        numErr++;                                          // число ошибок чтение по модбасу
 //       journal.jprintf(pP_TIME,cErrorRS485,name,err);     // Вывод кода ошибки в журнал
      }
  if (err!=OK)                                              // Ошибка модбаса
      {
       state=ERR_LINK_FC;                                  // признак потери связи с инвертором
       SETBIT1(flags,fErrFC);                              // Блок инвертора
       set_Error(err,name);                                 // генерация ошибки
       return err;                                          // Возврат
      }
//  else  if ((testMode==NORMAL)||(testMode==HARD_TEST))     //   Режим работа и хард тест, анализируем состояние,
//        if ((GETBIT(flags,fOnOff))&&(state!=3))                  // Не верное состояние
//         {
//         err=ERR_MODBUS_STATE;                            // Ошибка не верное состояние инвертора
//         journal.jprintf(" %s:Compressor ON and wrong read state: %d \n",name,state); 
//         set_Error(err,name);  
//         return err;                                        // Возврат
//         }
    // Состояние прочитали и оно правильное все остальное читаем
    _delay(FC_DELAY_READ);
    freqFC=read_0x03_32(MX2_CURRENT_FR);               // прочитать текущую частоту
    err=Modbus.get_err();                             // Скопировать ошибку
    if (err!=OK) {state=ERR_LINK_FC; }               // Ошибка выходим
    
    _delay(FC_DELAY_READ);
    power=read_0x03_16(MX2_POWER);                    // прочитать мощность
    err=Modbus.get_err();                             // Скопировать ошибку
    if (err!=OK) {state=ERR_LINK_FC;}                // Ошибка выходим
    
    _delay(FC_DELAY_READ);
    current=read_0x03_16(MX2_AMPERAGE);               // прочитать ток
    err=Modbus.get_err();                             // Скопировать ошибку
    if (err!=OK) {state=ERR_LINK_FC;}                // Ошибка выходим
#else // Аналоговое управление
    freqFC=FC;
    power=0;
    current=0;
#endif
return err;                            
}

// Команда ход на инвертор (целевая частота НЕ ВЫСТАВЛЯЕТСЯ)
// Может быть подана команда через реле и через модбас в зависимости от ключей компиляции
int8_t devOmronMX2::start_FC()
{
if (((testMode==NORMAL)||(testMode==HARD_TEST))&&(((FC<_data.minFreq)||(FC>_data.maxFreq)))) {journal.jprintf(" %s: Wrong frequency, ignore start\n",name);  return err;} // проверка частоты не в режиме теста
err=OK;
 #ifndef FC_ANALOG_CONTROL                                    // Не аналоговое управление
      #ifdef DEMO
             #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
                 HP.dRelay[RCOMP].set_ON();                // ПЛОХО через глобальную переменную
             #endif // FC_USE_RCOMP   
            if (err==OK) {SETBIT1(flags,fOnOff);startCompressor=rtcSAM3X8.unixtime(); journal.jprintf(" %s ON\n",name);}
            else {state=ERR_LINK_FC; SETBIT1(flags,fErrFC); set_Error(err,name);}               // генерация ошибки
      #else // DEMO
             // Боевая часть
            if (((testMode==NORMAL)||(testMode==HARD_TEST))&&(((!get_present())||(GETBIT(flags,fErrFC))))) return err;         // выходим если нет инвертора или он заблокирован по ошибке
          
           // set_target(startFreq,true);  // Запись в регистр инвертора стартовой частоты  НЕ всегда частота стартовая - супербойлер
           
            err=OK;
            if ((testMode==NORMAL)||(testMode==HARD_TEST))  //   Режим работа и хард тест, все включаем,
            {  
            #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
                HP.dRelay[RCOMP].set_ON();                // ПЛОХО через глобальную переменную
            #else                 // подать команду ход/стоп через модбас
                err= write_0x05_bit(MX2_START, true);   // Команда Ход
            #endif    
            }
            if (err==OK) {SETBIT1(flags,fOnOff);startCompressor=rtcSAM3X8.unixtime(); journal.jprintf(" %s ON\n",name);}
            else {state=ERR_LINK_FC; SETBIT1(flags,fErrFC); set_Error(err,name);}               // генерация ошибки
      #endif
  #else  //  FC_ANALOG_CONTROL
       #ifdef DEMO
            #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
                 HP.dRelay[RCOMP].set_ON();                // ПЛОХО через глобальную переменную
            #endif // FC_USE_RCOMP   
            SETBIT1(flags,fOnOff);startCompressor=rtcSAM3X8.unixtime(); journal.jprintf(" %s ON\n",name);
        #else // DEMO
             // Боевая часть
            if (((testMode==NORMAL)||(testMode==HARD_TEST))&&(((!get_present())||(GETBIT(flags,fErrFC))))) return err;   // выходим если нет инвертора или он заблокирован по ошибке
            err=OK;
            if ((testMode==NORMAL)||(testMode==HARD_TEST))  //   Режим работа и хард тест, все включаем,
            {  
            #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
                HP.dRelay[RCOMP].set_ON();                // ПЛОХО через глобальную переменную
            #else               
                state=ERR_LINK_FC; err=ERR_FC_CONF_ANALOG; SETBIT1(flags,fErrFC); set_Error(err,name);// Ошибка конфигурации
            #endif    
            }
            SETBIT1(flags,fOnOff);startCompressor=rtcSAM3X8.unixtime(); journal.jprintf(" %s ON\n",name);
      #endif 
  #endif    
return err;
}

// Команда стоп на инвертор Обратно код ошибки
int8_t devOmronMX2::stop_FC()
{
uint8_t i;	
err=OK;     
 #ifndef FC_ANALOG_CONTROL                                    // Не аналоговое управление
      #ifdef DEMO
            #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
               HP.dRelay[RCOMP].set_OFF();                // ПЛОХО через глобальную переменную
            #endif // FC_USE_RCOMP   
          if (err==OK) {SETBIT0(flags,fOnOff);startCompressor=0; journal.jprintf(" %s OFF\n",name);}
          else {state=ERR_LINK_FC; SETBIT1(flags,fErrFC); set_Error(err,name);}               // генерация ошибки
      #else // не DEMO
          if (!get_present()) return err; // если инвертора нет выходим
          // if  (((testMode==NORMAL)||(testMode==HARD_TEST))&&(((!get_present())||(GETBIT(flags,fErrFC))))) return err;// выходим если нет инвертора или он заблокирован по ошибке
          err=OK;   
          if ((testMode==NORMAL)||(testMode==HARD_TEST))      // Режим работа и хард тест, все включаем,
          {  
          #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп с проверкой выполнения
              HP.dRelay[RCOMP].set_OFF();            // ПЛОХО через глобальную переменную
               vTaskDelay(1000/ portTICK_PERIOD_MS); // задержка на прохождение команды
               state=read_0x03_16(MX2_STATE);        // 0:Начальное состояние, 2:Остановка 3:Вращение 4:Остановка с выбегом 5:Толчковый ход 6:Торможение  постоянным током 7:Выполнение  повторной попытки 8:Аварийное  отключение 9:Пониженное напряжение -1:Блокировка]
 /*             if ((state!=4)||(state!=2)||(state!=7)) { // если не тормозим то плохо, надо по модбасу рулить
              	 err=write_0x05_bit(MX2_START, false);   // подать команду ход/стоп через модбас
                  _delay(100);
              	 err=write_0x05_bit(MX2_START, false);   // дубль подать команду ход/стоп через модбас
              	 SETBIT1(flags,fErrFC);                  // Установить флаг блокировки
              	 err=ERR_FC_RCOMP;
                 set_Error(err,(char*)name);             // Подъем ошибки на верх и останов ТН
              	 journal.jprintf("$ERROR: it is not possible to stop the inverter via RCOMP, the inverter is blocked\n"); 
              	} */
      /*         for(i=0;i<FC_NUM_READ;i++)  // установить целевую частоту в 0
		            {
		              err=write_0x10_32(MX2_TARGET_FR,0);
		              if (err==OK) break;             // Команда выполнена
		              _delay(100);
		              journal.jprintf("%s: repeat set frequency 0.0 Hz\n",name);  // Выводим сообщение о повторной команде
		            }
	*/	            
          #else                  // подать команду ход/стоп через модбас
              err=write_0x05_bit(MX2_START, false);   // Команда стоп
          #endif   
           }
          if (err==OK) {SETBIT0(flags,fOnOff);startCompressor=0; journal.jprintf(" %s OFF\n",name);}
          else {state=ERR_LINK_FC; SETBIT1(flags,fErrFC); set_Error(err,name);}                          // генерация ошибки
      #endif
 #else  // FC_ANALOG_CONTROL 
      #ifdef DEMO
            #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
               HP.dRelay[RCOMP].set_OFF();                // ПЛОХО через глобальную переменную
            #endif // FC_USE_RCOMP   
            SETBIT0(flags,fOnOff);startCompressor=0; journal.jprintf(" %s OFF\n",name);
      #else // не DEMO
          if  (((testMode==NORMAL)||(testMode==HARD_TEST))&&(((!get_present())||(GETBIT(flags,fErrFC))))) return err;    // выходим если нет инвертора или он заблокирован по ошибке
          if ((testMode==NORMAL)||(testMode==HARD_TEST))      // Режим работа и хард тест, все включаем,
          {  
          #ifdef FC_USE_RCOMP   // Использовать отдельный провод для команды ход/стоп
              HP.dRelay[RCOMP].set_OFF();                    // ПЛОХО через глобальную переменную
          #else                  // подать команду ход/стоп через модбас
              state=ERR_LINK_FC; err=ERR_FC_CONF_ANALOG; SETBIT1(flags,fErrFC); set_Error(err,name);// Ошибка конфигурации
          #endif   
           }
          SETBIT0(flags,fOnOff);startCompressor=0; journal.jprintf(" %s OFF\n",name);
      #endif 
 #endif // FC_ANALOG_CONTROL 
return err;
}

// Получить параметр инвертора в виде строки, результат ДОБАВЛЯЕТСЯ в ret
void devOmronMX2::get_paramFC(char *var,char *ret)
{
    if(strcmp(var,fc_ON_OFF)==0)                { if (GETBIT(flags,fOnOff))  strcat(ret,(char*)cOne);else  strcat(ret,(char*)cZero); } else
    if(strcmp(var,fc_INFO)==0)                  {
    	                                        #ifndef FC_ANALOG_CONTROL  
    	                                        get_infoFC(ret);
    	                                        #else
                                                 strcat(ret, "|Данные не доступны, работа через аналоговый вход|;") ;
                                                #endif              
    	                                        } else
    if(strcmp(var,fc_NAME)==0)                  {  strcat(ret,name);             } else
    if(strcmp(var,fc_NOTE)==0)                  {  strcat(ret,note);             } else
    if(strcmp(var,fc_PIN)==0)                   {  _itoa(pin,ret);     } else
    if(strcmp(var,fc_PRESENT)==0)               { if (GETBIT(flags,fFC))  strcat(ret,(char*)cOne);else  strcat(ret,(char*)cZero); } else
    if(strcmp(var,fc_STATE)==0)                 {  _itoa(state,ret);   } else
    if(strcmp(var,fc_FC)==0)                    {  _ftoa(ret,(float)FC/100.0,2); } else
    if(strcmp(var,fc_cFC)==0)                   {  _ftoa(ret,(float)freqFC/100.0,2); } else
    if(strcmp(var,fc_cPOWER)==0)                {  _ftoa(ret,(float)power/10.0,1); } else
    if(strcmp(var,fc_INFO1)==0)                 {  _ftoa(ret,(float)power/10.0,1); strcat(ret, " кВт"); } else
    if(strcmp(var,fc_cCURRENT)==0)              {  _ftoa(ret,(float)current/100.0,2); } else
    if(strcmp(var,fc_AUTO_RESET_FAULT)==0)      {  strcat(ret,(char*)(GETBIT(_data.setup_flags,fAutoResetFault) ? cOne : cZero)); } else
    if(strcmp(var,fc_LogWork)==0)      			{  strcat(ret,(char*)(GETBIT(_data.setup_flags,fLogWork) ? cOne : cZero)); } else
    if(strcmp(var,fc_ANALOG)==0)                { // Флаг аналогового управления
		                                        #ifdef FC_ANALOG_CONTROL                                                    
		                                         strcat(ret,(char*)cOne);
		                                        #else
		                                         strcat(ret,(char*)cZero);
		                                        #endif
                                                } else
    if(strcmp(var,fc_DAC)==0)                   {  _itoa(dac,ret);          } else
    #ifdef FC_ANALOG_CONTROL
    if(strcmp(var,fc_LEVEL0)==0)                {  _itoa(level0,ret);       } else
    if(strcmp(var,fc_LEVEL100)==0)              {  _itoa(level100,ret);     } else
    if(strcmp(var,fc_LEVELOFF)==0)              {  _itoa(levelOff,ret);     } else
    #endif
    if(strcmp(var,fc_BLOCK)==0)                 { if (GETBIT(flags,fErrFC))  strcat(ret,(char*)cOne);else  strcat(ret,(char*)cZero); } else
    if(strcmp(var,fc_ERROR)==0)                 {  _itoa(err,ret);          } else
    if(strcmp(var,fc_UPTIME)==0)                {  _itoa(_data.Uptime,ret); } else   // вывод в секундах
    if(strcmp(var,fc_PID_STOP)==0)              {  _itoa(_data.PidStop,ret);          } else
    if(strcmp(var,fc_DT_COMP_TEMP)==0)          {  _ftoa(ret,(float)_data.dtCompTemp/100.0,2); } else // градусы
	if(strcmp(var,fc_PID_FREQ_STEP)==0)         {  _ftoa(ret,(float)_data.PidFreqStep/100.0,2); } else // Гц
	if(strcmp(var,fc_START_FREQ)==0)            {  _ftoa(ret,(float)_data.startFreq/100.0,2); } else // Гц
	if(strcmp(var,fc_START_FREQ_BOILER)==0)     {  _ftoa(ret,(float)_data.startFreqBoiler/100.0,2); } else // Гц
	if(strcmp(var,fc_MIN_FREQ)==0)              {  _ftoa(ret,(float)_data.minFreq/100.0,2); } else // Гц
	if(strcmp(var,fc_MIN_FREQ_COOL)==0)         {  _ftoa(ret,(float)_data.minFreqCool/100.0,2); } else // Гц
	if(strcmp(var,fc_MIN_FREQ_BOILER)==0)       {  _ftoa(ret,(float)_data.minFreqBoiler/100.0,2); } else // Гц
	if(strcmp(var,fc_MIN_FREQ_USER)==0)         {  _ftoa(ret,(float)_data.minFreqUser/100.0,2); } else // Гц
	if(strcmp(var,fc_MAX_FREQ)==0)              {  _ftoa(ret,(float)_data.maxFreq/100.0,2); } else // Гц
	if(strcmp(var,fc_MAX_FREQ_COOL)==0)         {  _ftoa(ret,(float)_data.maxFreqCool/100.0,2); } else // Гц
	if(strcmp(var,fc_MAX_FREQ_BOILER)==0)       {  _ftoa(ret,(float)_data.maxFreqBoiler/100.0,2); } else // Гц
	if(strcmp(var,fc_MAX_FREQ_USER)==0)         {  _ftoa(ret,(float)_data.maxFreqUser/100.0,2); } else // Гц
	if(strcmp(var,fc_STEP_FREQ)==0)             {  _ftoa(ret,(float)_data.stepFreq/100.0,2); } else // Гц
	if(strcmp(var,fc_STEP_FREQ_BOILER)==0)      {  _ftoa(ret,(float)_data.stepFreqBoiler/100.0,2); } else // Гц
    if(strcmp(var,fc_DT_TEMP)==0)               {  _ftoa(ret,(float)_data.dtTemp/100.0,2); } else // градусы
    if(strcmp(var,fc_DT_TEMP_BOILER)==0)        {  _ftoa(ret,(float)_data.dtTempBoiler/100.0,2); } else // градусы
    if(strcmp(var,fc_MB_ERR)==0)        		{  _itoa(numErr, ret); } else
   	if(strcmp(var,fc_FC_TIME_READ)==0)   		{  _itoa(FC_TIME_READ, ret); } else
   		strcat(ret,(char*)cInvalid);
}
   


// Установить параметр инвертора из строки
boolean devOmronMX2::set_paramFC(char *var, float x)
{
    if(strcmp(var,fc_ON_OFF)==0)                { if (x==0) stop_FC();else start_FC();return true;  } else 
    if(strcmp(var,fc_FC)==0)                    { if((x*100>=_data.minFreqUser)&&(x*100<=_data.maxFreqUser)){set_target(x*100,true, _data.minFreqUser, _data.maxFreqUser); return true; }else return false; } else
    if(strcmp(var,fc_AUTO_RESET_FAULT)==0)      { if (x==0) SETBIT0(_data.setup_flags,fAutoResetFault);else SETBIT1(_data.setup_flags,fAutoResetFault);return true;  } else // для Омрона код не написан
    if(strcmp(var,fc_LogWork)==0)               { _data.setup_flags = (_data.setup_flags & ~(1<<fLogWork)) | ((x!=0)<<fLogWork); return true;  } else

    #ifdef FC_ANALOG_CONTROL
    if(strcmp(var,fc_LEVEL0)==0)                { if ((x>=0)&&(x<=4096)) { level0=x; return true;} else return false;      } else 
    if(strcmp(var,fc_LEVEL100)==0)              { if ((x>=0)&&(x<=4096)) { level100=x; return true;} else return false;    } else 
    if(strcmp(var,fc_LEVELOFF)==0)              { if ((x>=0)&&(x<=4096)) { levelOff=x; return true;} else return false;    } else 
    #endif
    if(strcmp(var,fc_BLOCK)==0)                 { SemaphoreGive(xModbusSemaphore); // отдать семафор ВСЕГДА
                                                if (x==0) { SETBIT0(flags,fErrFC); note=(char*)noteFC_OK; }
                                                else      { SETBIT1(flags,fErrFC); note=(char*)noteFC_NO; }
                                                return true;            
                                                } else  
    if(strcmp(var,fc_UPTIME)==0)                { if((x>=3)&&(x<600)){_data.Uptime=x;return true; } else return false; } else   // хранение в сек
    if(strcmp(var,fc_PID_STOP)==0)              { if((x>50)&&(x<100)){_data.PidStop=x;return true; } else return false;  } else 
    if(strcmp(var,fc_DT_COMP_TEMP)==0)          { if((x>1)&&(x<25)){_data.dtCompTemp=x*100;return true; } else return false; } else // градусы

	if(strcmp(var,fc_PID_FREQ_STEP)==0)         { if((x>0)&&(x<5)){_data.PidFreqStep=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_START_FREQ)==0)            { if((x>20)&&(x<120)){_data.startFreq=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_START_FREQ_BOILER)==0)     { if((x>20)&&(x<150)){_data.startFreqBoiler=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MIN_FREQ)==0)              { if((x>20)&&(x<80)){_data.minFreq=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MIN_FREQ_COOL)==0)         { if((x>20)&&(x<80)){_data.minFreqCool=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MIN_FREQ_BOILER)==0)       { if((x>20)&&(x<80)){_data.minFreqBoiler=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MIN_FREQ_USER)==0)         { if((x>20)&&(x<80)){_data.minFreqUser=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MAX_FREQ)==0)              { if((x>40)&&(x<200)){_data.maxFreq=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MAX_FREQ_COOL)==0)         { if((x>40)&&(x<200)){_data.maxFreqCool=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MAX_FREQ_BOILER)==0)       { if((x>40)&&(x<200)){_data.maxFreqBoiler=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_MAX_FREQ_USER)==0)         { if((x>40)&&(x<200)){_data.maxFreqUser=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_STEP_FREQ)==0)             { if((x>0.2)&&(x<10)){_data.stepFreq=x*100;return true; } else return false; } else // Гц
	if(strcmp(var,fc_STEP_FREQ_BOILER)==0)      { if((x>0.2)&&(x<10)){_data.stepFreqBoiler=x*100;return true; } else return false; } // Гц

	if(strcmp(var,fc_DT_TEMP)==0)               { if((x>0)&&(x<10)){_data.dtTemp=x*100;return true; } else return false; } else // градусы
    if(strcmp(var,fc_DT_TEMP_BOILER)==0)        { if((x>0)&&(x<10)){_data.dtTempBoiler=x*100;return true; } else return false; } else // градусы
    return false;
}

	
 
// Получить информацию о частотнике, информация добавляется в buf
char * devOmronMX2::get_infoFC(char *buf)
{
#ifndef FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ
  if(!HP.dFC.get_present()) { strcat(buf,"|Данные не доступны (нет инвертора)|;"); return buf;}          // Инвертора нет в конфигурации
  if(HP.dFC.get_blockFC())  { strcat(buf,"|Данные не доступны (нет связи по Modbus, инвертор заблокирован)|;"); return buf;}  // Инвертор заблокирован
  int8_t i;  
       strcat(buf,"-|Состояние инвертора [0:Начальное состояние, 2:Остановка 3:Вращение 4:Остановка с выбегом 5:Толчковый ход 6:Торможение  постоянным током ");strcat(buf,"7:Выполнение  повторной попытки 8:Аварийное  отключение 9:Пониженное напряжение -1:Блокировка]|");_itoa(read_0x03_16(MX2_STATE),buf);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d001|Контроль выходной частоты (Гц)|");_ftoa(buf,(float)read_0x03_32(MX2_CURRENT_FR)/100.0,2);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d003|Контроль выходного тока (А)|");_ftoa(buf,(float)read_0x03_16(MX2_AMPERAGE)/100.0,2);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d014|Контроль мощности (кВт)|");_ftoa(buf,(float)read_0x03_16(MX2_POWER)/10.0,1);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d013|Контроль выходного напряжения (В)|");_ftoa(buf,(float)read_0x03_16(MX2_VOLTAGE)/10.0,1);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d015|Контроль ватт-часов (кВт/ч)|");_ftoa(buf,(float)read_0x03_32(MX2_POWER_HOUR)/10.0,1);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d016|Контроль времени наработки в режиме \"Ход\" (ч)|");_itoa(read_0x03_32(MX2_HOUR),buf);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d017|Контроль времени наработки при включенном питании (ч)|");_itoa(read_0x03_32(MX2_HOUR1),buf);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d018|Контроль температуры радиатора (°С)|");_ftoa(buf,(float)read_0x03_16(MX2_TEMP)/10.0,2);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d102|Контроль напряжения  постоянного тока (В)|");_ftoa(buf,(float)read_0x03_16(MX2_VOLTAGE_DC)/10.0,1);strcat(buf,";");
       _delay(FC_DELAY_READ);
       strcat(buf,"d080|Счетчик аварийных отключений (Шт)|");_itoa(read_0x03_16(MX2_NUM_ERR),buf);strcat(buf,";");
       for(i=0;i<6;i++)  // Скан по ошибкам
          {
          strcat(buf,"d0");_itoa(81+i,buf);strcat(buf,"|Состояние в момент ошибки ");
          read_0x03_error(MX2_ERROR1+i*0x0a);
          // Формирование ответа в строке
          strcat(buf,"[F:");  _ftoa(buf,(float)error.MX2.fr/100.0,2);
          strcat(buf," I:");  _ftoa(buf,(float)error.MX2.cur/100.0,2);
          strcat(buf," V:");  _ftoa(buf,(float)error.MX2.vol/10.0,2);
          strcat(buf," T1:"); _itoa(error.MX2.time1,buf);
          strcat(buf," T2:"); _itoa(error.MX2.time2,buf);
          strcat(buf,"] Код ошибки:|");
          if(error.MX2.code<10) strcat(buf,"E0"); else strcat(buf,"E");
          _itoa(error.MX2.code,buf);strcat(buf,"."); _itoa(error.MX2.status,buf);
          strcat(buf,";");   
          }  
#endif          
return buf;                              
}          
// Сброс ошибок инвертора по модбасу
boolean devOmronMX2::reset_errorFC()                   
{
#ifndef FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ
  write_0x06_16(MX2_INIT_DEF, 0x01);       // задать режим иннициализации - стирание ошибок
  _delay(FC_DELAY_READ);
  if((read_0x03_16(MX2_INIT_DEF)==0x01)&&(err=OK))   // подать команду на инициализацию, если записано стирание только ошибок
    {
    write_0x06_16(MX2_INIT_RUN, 0x01);       
    journal.jprintf("Reset error %s\r\n",name);
    }
  else journal.jprintf("$WARNING: bad read from MX2_INIT_DEF, no reset error\r\n");
#endif
if (err==OK) return true;  else return false;   
}

// Сброс инвертора через модбас
boolean devOmronMX2::reset_FC() 
{
#ifndef FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ
  write_0x05_bit(MX2_RESET, true);               // подать команду на сброс по модбас
  journal.jprintf("Reset %s use Modbus\r\n",name);  
#endif
if (err==OK) return true;  else return false;                          
}

// Текущее состояние инвертора
// 
int16_t devOmronMX2::read_stateFC()
{
#ifndef FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ
  state=read_0x03_16(MX2_STATE);  // прочитать состояние
  if(GETBIT(_data.setup_flags,fLogWork) && GETBIT(flags, fOnOff)) {
			journal.jprintf(pP_TIME, "FC: %Xh, %.2fHz, %.2fA, %.2fkW\n", state, (float)freqFC/100.0, (float)current/100.0, (float)get_power()/1000.0);}
  return state;
#else
  return 0;
#endif  
} 

// Tемпература радиатора
int16_t devOmronMX2::read_tempFC()
{
#ifndef FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ
  return read_0x03_16(MX2_TEMP);
#else
  return 0;
#endif  
} 
// Функции общения по модбас инвертора  Чтение регистров
#ifndef FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ
    // Чтение отдельного бита в регистр cmd возвращает код ,  ошибка обновляется
    // Реалезовано FC_NUM_READ попыток чтения/записи в инвертор
    boolean devOmronMX2::read_0x01_bit(uint16_t cmd)
    { uint8_t i;
      boolean result;
      err=OK;  
      if ((!get_present())||(GETBIT(flags,fErrFC))) return false;             // выходим если нет инвертора или он заблокирован по ошибке
      for(i=0;i<FC_NUM_READ;i++)   // делаем FC_NUM_READ попыток чтения Чтение состояния инвертора, при ошибке генерация общей ошибки ТН и останов
         { 
         err=Modbus.readCoil(FC_MODBUS_ADR,cmd-1, &result);              // послать запрос, Нумерация регистров MX2 с НУЛЯ!!!!
         if (err==OK) break;                                                // Прочитали удачно
         _delay(FC_DELAY_REPEAT);
         journal.jprintf(cErrorRS485,name,__FUNCTION__,err);                // Выводим сообщение о повторном чтении
         numErr++;                                                          // число ошибок чтение по модбасу
 //        journal.jprintf(pP_TIME,cErrorRS485,name,err);                     // Вывод кода ошибки в журнал
         }
    
      check_blockFC();                                                     // проверить необходимость блокировки
      return result;  
    }
    // Функция 0х03 прочитать 2 байта, возвращает значение, ошибка обновляется
    // Реализовано FC_NUM_READ попыток чтения/записи в инвертор
    int16_t  devOmronMX2::read_0x03_16(uint16_t cmd)
    {   uint8_t i;
        uint16_t result;  
        err=OK;
        if ((!get_present())||(GETBIT(flags,fErrFC))) return 0;                  // выходим если нет инвертора или он заблокирован по ошибке
    
        for(i=0;i<FC_NUM_READ;i++)   // делаем FC_NUM_READ попыток чтения Чтение состояния инвертора, при ошибке генерация общей ошибки ТН и останов
            { 
            err=Modbus.readHoldingRegisters16(FC_MODBUS_ADR,cmd-1,&result);  // Послать запрос, Нумерация регистров MX2 с НУЛЯ!!!!
            if (err==OK) break;                                                 // Прочитали удачно
            _delay(FC_DELAY_REPEAT);
             journal.jprintf(cErrorRS485,name,__FUNCTION__,err);                // Выводим сообщение о повторном чтении
             numErr++;                                                          // число ошибок чтение по модбасу
   //         journal.jprintf(pP_TIME,cErrorRS485,name,err);                     // Вывод кода ошибки в журнал
            }
            check_blockFC();                                                   // проверить необходимость блокировки
            return result;
             
    }
    
    // Функция 0х03 прочитать 4 байта
    // Реализовано FC_NUM_READ попыток чтения/записи в инвертор
    uint32_t devOmronMX2::read_0x03_32(uint16_t cmd)
    {
        uint8_t i;
        uint32_t result;  
        err=OK;
        if ((!get_present())||(GETBIT(flags,fErrFC))) return 0;            // выходим если нет инвертора или он заблокирован по ошибке
        for(i=0;i<FC_NUM_READ;i++)   // делаем FC_NUM_READ попыток чтения Чтение состояния инвертора, при ошибке генерация общей ошибки ТН и останов
          { 
           err=Modbus.readHoldingRegisters32(FC_MODBUS_ADR,cmd-1,&result);  // послать запрос, Нумерация регистров MX2 с НУЛЯ!!!!
           if (err==OK) break;                                                 // Прочитали удачно
          _delay(FC_DELAY_REPEAT);
          journal.jprintf(cErrorRS485,name,__FUNCTION__,err);                 // Выводим сообщение о повторном чтении
          numErr++;                                                           // число ошибок чтение по модбасу
    //       journal.jprintf(pP_TIME,cErrorRS485,name,err);                   // Вывод кода ошибки в журнал
          }
        check_blockFC();                                                      // проверить необходимость блокировки
        return result;
    }
    
    // Функция Modbus 0х03 описание ошибки num НУМЕРАЦИЯ с 0 (общая длина данных 10 слов по 2 байта)
    // Возвращает код ошибки и в буфер кладет описание
    // Реализовано FC_NUM_READ попыток чтения/записи в инвертор
    int16_t devOmronMX2::read_0x03_error(uint16_t cmd)  
    { uint8_t i;
      uint16_t tmp;
      err=OK;
      if ((!get_present())||(GETBIT(flags,fErrFC))) return err;              // выходим если нет инвертора или он заблокирован по ошибке
      for(i=0;i<0x0a;i++) error.inputBuf[i]=0;
      for(i=0;i<FC_NUM_READ;i++)   // делаем FC_NUM_READ попыток чтения Чтение состояния инвертора, при ошибке генерация общей ошибки ТН и останов
         { 
         err = Modbus.readHoldingRegistersNN(FC_MODBUS_ADR,cmd-1,0x0a,error.inputBuf);  // послать запрос, Нумерация регистров с НУЛЯ!!!!
         if (err==OK) break;                                                 // Прочитали удачно
         _delay(FC_DELAY_REPEAT);
         journal.jprintf(cErrorRS485,name,__FUNCTION__,err);                 // Выводим сообщение о повторном чтении
          numErr++;                                                          // число ошибок чтение по модбасу
  //        journal.jprintf(pP_TIME,cErrorRS485,name,err);                     // Вывод кода ошибки в журнал
         }
      if (err==OK) // Для времен переставить местами слова (2 байта) т.е сначала идет старшие 2 байта потом младшие
      {
        tmp=error.inputBuf[6]; error.inputBuf[6]=error.inputBuf[7]; error.inputBuf[7]=tmp; // Общее время наработки в режиме «Ход» к моменту отключения
        tmp=error.inputBuf[8]; error.inputBuf[8]=error.inputBuf[9]; error.inputBuf[9]=tmp; // Общее время работы ПЧ при включенном питании в момент отключения выхода
      }
      check_blockFC();                                                   // проверить необходимость блокировки
      return err;
    }
    
    // Запись отдельного бита в регистр cmd возвращает код ошибки
    // Реализовано FC_NUM_READ попыток чтения/записи в инвертор
    int8_t devOmronMX2::write_0x05_bit(uint16_t cmd, boolean f)
    { uint8_t i;
      err=OK;
      if ((!get_present())||(GETBIT(flags,fErrFC))) return err;     // выходим если нет инвертора или он заблокирован по ошибке
      for(i=0;i<FC_NUM_READ;i++)   // делаем FC_NUM_READ попыток записи
         {   
            if (f) err=Modbus.writeSingleCoil(FC_MODBUS_ADR,cmd-1,1);   // послать запрос, Нумерация регистров с НУЛЯ!!!!
            else   err=Modbus.writeSingleCoil(FC_MODBUS_ADR,cmd-1,0);
            if (err==OK) break;                                            // Записали удачно
            _delay(FC_DELAY_REPEAT);
           journal.jprintf(cErrorRS485,name,__FUNCTION__,err);             // Выводим сообщение о повторном чтении
           numErr++;                                                       // число ошибок чтение по модбасу
  //         journal.jprintf(pP_TIME,cErrorRS485,name,err);                  // Вывод кода ошибки в журнал
         }
      check_blockFC();                                                    // проверить необходимость блокировки
      return err;
    }
    // Запись данных (2 байта) в регистр cmd возвращает код ошибки
    // Реализовано FC_NUM_READ попыток чтения/записи в инвертор
    int8_t devOmronMX2::write_0x06_16(uint16_t cmd, uint16_t data)
    { uint8_t i;
      err=OK;
      if ((!get_present())||(GETBIT(flags,fErrFC))) return err;              // выходим если нет инвертора или он заблокирован по ошибке
       for(i=0;i<FC_NUM_READ;i++)                                          // делаем FC_NUM_READ попыток записи
         {
           err=Modbus.writeHoldingRegisters16(FC_MODBUS_ADR,cmd-1,data);  // послать запрос, Нумерация регистров с НУЛЯ!!!!
           if (err==OK) break;                                               // Записали удачно
           _delay(FC_DELAY_REPEAT);
           journal.jprintf(cErrorRS485,name,__FUNCTION__,err);                // Выводим сообщение о повторном чтении
           numErr++;                                                          // число ошибок чтение по модбасу
   //        journal.jprintf(pP_TIME,cErrorRS485,name,err);                     // Вывод кода ошибки в журнал
         }
      check_blockFC();                                                      // проверить необходимость блокировки
      return err;
      
    }
    // Запись данных (4 байта) в регистр cmd возвращает код ошибки
    int8_t devOmronMX2::write_0x10_32(uint16_t cmd, uint32_t data)
    { uint8_t i;
      err=OK;
      if ((!get_present())||(GETBIT(flags,fErrFC))) return err;             // выходим если нет инвертора или он заблокирован по ошибке
      for(i=0;i<FC_NUM_READ;i++)                                          // делаем FC_NUM_READ попыток записи
         {  
           err=Modbus.writeHoldingRegisters32(FC_MODBUS_ADR, cmd-1, data);// послать запрос, Нумерация регистров с НУЛЯ!!!!
           if (err==OK) break;                                               // Записали удачно
           _delay(FC_DELAY_REPEAT);
           journal.jprintf(cErrorRS485,name,__FUNCTION__,err);                // Выводим сообщение о повторном чтении
           numErr++;                                                          // число ошибок чтение по модбасу
  //         journal.jprintf(pP_TIME,cErrorRS485,name,err);                     // Вывод кода ошибки в журнал
         }
      check_blockFC();                                                       // проверить необходимость блокировки
      return err; 
    }
#endif  // FC_ANALOG_CONTROL    // НЕ АНАЛОГОВОЕ УПРАВЛЕНИЕ

#endif // НЕ FC_VACON

// ------------------------ Счетчик SDM ---------------------------------------
// Инициализация счетчика и проверка и если надо программирование
int8_t devSDM::initSDM()
{
  err=OK;                                        // Ошибок нет
  numErr=0;                                      // счетчик 0
  Voltage=0.0;                                   // Напряжение
  Current=0.0;                                   // Ток
  AcPower=0.0;                                   // активная мощность
  AcEnergy=0.0;                                  // Суммараная активная энергия
  flags=0x00;
  // Настройки
  settingSDM.maxVoltage=SDM_MAX_VOLTAGE;         // максимальное напряжение (вольты) иначе ошибка если 0 то не работает
  settingSDM.minVoltage=SDM_MIN_VOLTAGE;         // минимальное напряжение (вольты) иначе ПРЕДУПРЕЖДЕНИЕ если 0 то не работает
  settingSDM.maxPower=SDM_MAX_POWER;             // максимальная мощность (ватты) напряжение иначе ошибка если 0 то не работает
  name=(char*)nameSDM;
  note=(char*)noteSDM_NONE;  
      
  #ifdef USE_ELECTROMETER_SDM
      if(!Modbus.get_present())                  // modbus отсутствует
      {
      journal.jprintf("%s: modbus not found, block.\n",name); 
      SETBIT0(flags,fSDM);                           // счетчик не представлен
      SETBIT0(flags,fSDMLink);
      err=ERR_NO_MODBUS;
      }
      else
      {
      SETBIT1(flags,fSDM);                           // счетчик представлен
      note=(char*)noteSDM;
      uplinkSDM();                                  // проверить связь со счетчиком
      }
  #else
      SETBIT0(flags,fSDM);                           // счетчик не представлен
      SETBIT0(flags,fSDMLink);
      note=(char*)noteSDM_NONE;
  #endif
   // инициализация статистики
#ifndef MIN_RAM_CHARTS
  ChartVoltage.init(GETBIT(flags,fSDM));               // Статистика по напряжению
  ChartCurrent.init(GETBIT(flags,fSDM));               // Статистика по току
#endif
//  sAcPower.init(GETBIT(flags,fSDM));               // Статистика по активная мощность
//  sRePower.init(GETBIT(flags,fSDM));               // Статистика по Реактивная мощность
  ChartPower.init(GETBIT(flags,fSDM));                 // Статистика по Полная мощность
 // ChartPowerFactor.init(GETBIT(flags,fSDM));           // Статистика по Коэффициент мощности
 return err;
}

// Проверить связь со счетчиком предполагается что модбас уже нинициализирован читаем из регистра скорость
// Выводит сообщеиня в журнал и устанавливает флаг связи
boolean  devSDM::uplinkSDM() 
{
  float band;
  int8_t i;
  for(i=0;i<SDM_NUM_READ;i++)   // делаем SDM_NUM_READ попыток чтения
    {
     if ((Modbus.readHoldingRegistersFloat(SDM_MODBUS_ADR,SDM_BAUD_RATE,&band)==OK)&&(band==SDM_SPEED)) {SETBIT1(flags,fSDMLink); journal.jprintf("%s, found, link OK, band rate:%.0f modbus address:%d\n",name,band,SDM_MODBUS_ADR);  return true;}
     else { SETBIT0(flags,fSDMLink); journal.jprintf("%s, no connect.\n",name);}
     _delay(SDM_DELAY_REPEAD);
      WDT_Restart(WDT);                                                            // Сбросить вачдог
    }
 SETBIT0(flags,fSDMLink);                                                             // связи нет
 return false;   
}

// перепрограммировать счетчик на требуемые параметры связи SDM_SPEED SDM_MODBUS_ADR c DEFAULT_SDM_SPEED DEFAULT_SDM_MODBUS_ADR
// При программировании параметры сразу начинают рабоать
boolean  devSDM::progConnect()
{
  float band; 
  journal.jprintf("%s: Setting band rate and modbus address.\n",name); 
  // 1. Проверка
  if ((Modbus.readHoldingRegistersFloat(SDM_MODBUS_ADR,SDM_BAUD_RATE,&band)==OK)&&(band==SDM_SPEED)) {SETBIT1(flags,fSDMLink); journal.jprintf(" Setting %s are correct, programming is not required\n",name);  return true;} 
  
  // 2. Установка скорости по умолчанию
  
  MODBUS_PORT_NUM.begin(DEFAULT_SDM_SPEED,MODBUS_PORT_CONFIG);            // SERIAL_8N1 - настройки порта для счетчика из коробки
  err=OK;
  
  // 3. На DEFAULT_SDM_SPEED скорости установить правильный адрес SDM_MODBUS_ADR
  if( (err=Modbus.writeHoldingRegistersFloat(DEFAULT_SDM_MODBUS_ADR,SDM_ADR_MODBUS,SDM_MODBUS_ADR))==OK)  journal.jprintf("%s: Set address  %d.\n",name,SDM_MODBUS_ADR);  // установить требуемемый адрес
  else  journal.jprintf("%s: Setting address error, code %d.\n",name,err); 
  
  // 4. На правильный адрес SDM_MODBUS_ADR установит желаемую скорость SDM_SPEED
  if( (err=Modbus.writeHoldingRegistersFloat(SDM_MODBUS_ADR, SDM_BAUD_RATE,SDM_SPEED))==OK)  journal.jprintf("%s: Set band rate (0=2400bps 1=4800bps 2=9600bps 5=1200bps) %d.\n",name,SDM_SPEED);  // установить требуемую скорость
  else  journal.jprintf("%s: Setting band rate error, code %d.\n",name,err);
  
  // 5. Порт обратно в требуемые настройки
  MODBUS_PORT_NUM.begin(MODBUS_PORT_SPEED,MODBUS_PORT_CONFIG);                 // SERIAL_8N1 - настройки по умолчанию

  // 6. Вывод результатов
  if (err==OK) { journal.jprintf("%s: Programming is Ok\n",name); uplinkSDM(); return true;}  // Надо сбросить счетчик
  else { journal.jprintf("%s: Programming is wrong, no link\n",name); return false; }
}                           

// Прочитать инфо с счетчика, group: 0 - основная (при каждом цикле); 2 - через SDM_READ_PERIOD
int8_t devSDM::get_readState(uint8_t group)
{
	static float tmp;
	int8_t i;
	if((!GETBIT(flags,fSDM))||(!GETBIT(flags,fSDMLink))) return err;  // Если нет счетчика или нет связи выходим
	// Чтение состояния счетчика,
	err=OK;
	for(i=0; i<SDM_NUM_READ; i++)   // делаем SDM_NUM_READ попыток чтения
	{
		// Читаем значения счетчика
		if(group == 0) {
			err=Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_VOLTAGE,&tmp);   // Напряжение
			if(err==OK) { Voltage=tmp; group = 1; } else goto xErr;
		}
		if(group == 1) {
			err=Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_AC_POWER,&tmp);  // Активная мощность
			if(err==OK) AcPower=tmp; else goto xErr;
		} else if(group == 2) {
			err=Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_CURRENT,&tmp);   // Ток
			if(err == OK) { Current=tmp; group = 3; } else goto xErr;
		}
		if(group == 3) {
			err=Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_AC_ENERGY,&tmp); // Суммарная активная энергия
			if(err==OK) AcEnergy=tmp;
		}
		if(err == OK) break;
xErr:
#ifdef SPOWER
		HP.sInput[SPOWER].Read(true);
        if(HP.sInput[SPOWER].is_alarm()) return err;
#endif
		numErr++;                  // число ошибок чтение по модбасу
		if(GETBIT(HP.Option.flags, fSDMLogErrors)) {
			journal.jprintf(pP_TIME, "%s: Read #%d error %d, repeat...\n", name, group, err);      // Выводим сообщение о повторном чтении
		}
		WDT_Restart(WDT);          // Сбросить вачдог
		_delay(SDM_DELAY_REPEAD);  // Чтение не удачно, делаем паузу
	}
	if (err==OK)
	{
		// Serial.println((int)(Voltage*100));
		if ((settingSDM.maxVoltage>1)&&(settingSDM.maxVoltage< Voltage)) {err=ERR_MAX_VOLTAGE;set_Error(err,name);return err; }       // Контроль входного напряжения
		if ((settingSDM.maxPower>1)&&(settingSDM.maxPower< AcPower))     {err=ERR_MAX_POWER;set_Error(err,name);return err; }         // Контроль мощности потребления
		if ((settingSDM.minVoltage>1)&&(settingSDM.minVoltage>Voltage) ) {HP.message.setMessage(pMESSAGE_WARNING,(char*)"Напряжение сети ниже нормы",(int)Voltage);return err; } // сформировать уведомление о низком напряжени
		return err;                       // все прочиталось, выходим
	}
	SETBIT0(flags,fSDMLink);             // связь со счетчиком потеряна
	journal.jprintf(pP_TIME, "%s: Read #%d error %d!\n", name, group, err);
	// set_Error(err,name);              // генерация ошибки    НЕТ счетчик не критичен
	return err;
}

// Получить параметр счетчика в виде строки
char* devSDM::get_paramSDM(char *var, char *ret)           
{
   static float tmp;

   if(strcmp(var,sdm_NAME)==0){         return strcat(ret,(char*)name);                                         }else      // Имя счетчика
   if(strcmp(var,sdm_NOTE)==0){         return strcat(ret,(char*)note);                                         }else      // Описание счетчика
   if(strcmp(var,sdm_ERRORS)==0){    	return _itoa(numErr,ret);				                                }else      // Ошибок modbus
   if(strcmp(var,sdm_MAX_VOLTAGE)==0){  return _itoa(settingSDM.maxVoltage,ret);                                }else      // мах напряжение контроля напряжения
   if(strcmp(var,sdm_MIN_VOLTAGE)==0){  return _itoa(settingSDM.minVoltage,ret);                                }else      // min напряжение контроля напряжения
   if(strcmp(var,sdm_MAX_POWER)==0){    return _itoa(settingSDM.maxPower,ret);                                  }else      // максимальаня мощность контроля мощности
   if(strcmp(var,sdm_VOLTAGE)==0){      _ftoa(ret,(float)Voltage,2); return ret;                         }else      // Напряжение
   if(strcmp(var,sdm_CURRENT)==0){      _ftoa(ret,(float)Current,2); return ret;                         }else      // Ток
   if(strcmp(var,sdm_ACPOWER)==0){      _ftoa(ret,(float)AcPower,2);  return ret;                        }else      // Активная мощность
   if(strcmp(var,sdm_ACENERGY)==0){     _ftoa(ret,(float)AcEnergy,2); return ret;                        }else      // Суммарная активная энергия
   if(strcmp(var,sdm_LINK)==0){         if (GETBIT(flags,fSDMLink)) return strcat(ret,(char*)cYes); else return strcat(ret,(char*)cNo);}       // Cостояние связи со счетчиком
   else {
	   if(GETBIT(flags,fSDMLink)) {
//		   if(strcmp(var,sdm_CURRENT)==0){
//			   Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_CURRENT, &tmp);
//			   _ftoa(ret, tmp, 2);																			   }else       // Ток
		   if(strcmp(var,sdm_REPOWER)==0){
			   Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_RE_POWER, &tmp);
			   _ftoa(ret, tmp, 2);                                     											}else      // Реактивная мощность
		   if(strcmp(var,sdm_POWER)==0){
			   Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_POWER, &tmp);
			   _ftoa(ret, tmp, 2);																				}else      // Полная мощность
		   if(strcmp(var,sdm_POW_FACTOR)==0){
			   Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_POW_FACTOR, &tmp);
			   _ftoa(ret, tmp, 2);																				}else      // Коэффициент мощности
		   if(strcmp(var,sdm_PHASE)==0){
			   Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_PHASE, &tmp);
			   _ftoa(ret, tmp, 2);                                       										}else      // Угол фазы (градусы)
		   if(strcmp(var,sdm_FREQ)==0){
			   Modbus.readInputRegistersFloat(SDM_MODBUS_ADR, SDM_FREQUENCY, &tmp);
			   _ftoa(ret, tmp, 2);																				}         // Частота
	   }
	   return ret;
   }
   return strcat(ret,(char*)cInvalid);
}

// Установить параметр счетчика в виде строки
boolean devSDM::set_paramSDM(char *var,char *c)        
 {
  int16_t x=atoi(c);
   if(strcmp(var,sdm_MAX_VOLTAGE)==0){   if ((x>=0)&&(x<=400)) {settingSDM.maxVoltage=(uint16_t)x;return true;} else  return false; }else      // мах напряжение контроля напряжения
   if(strcmp(var,sdm_MIN_VOLTAGE)==0){   if ((x>=0)&&(x<=400)) {settingSDM.minVoltage=(uint16_t)x;return true;} else  return false; }else      // min напряжение контроля напряжения
   if(strcmp(var,sdm_MAX_POWER)==0){     if ((x>=0)&&(x<=25000)){settingSDM.maxPower=(uint16_t)x;  return true;} else  return false;}else      // максимальаня мощность контроля мощности
   return false;
 }

// МОДБАС Устройство ----------------------------------------------------------
// функции обратного вызова
static uint8_t Modbus_Entered_Critical = 0;
static inline void idle() // задержка между чтениями отдельных байт по Modbus
    {
//		delay(1);		// Не отдает время другим задачам
		_delay(1);		// Отдает время другим задачам
    }
static inline void preTransmission() // Функция вызываемая ПЕРЕД началом передачи
    {
      #ifdef PIN_MODBUS_RSE
      digitalWriteDirect(PIN_MODBUS_RSE, HIGH);
      #endif
      Modbus_Entered_Critical = TaskSuspendAll(); // Запрет других задач во время передачи по Modbus
    }
static inline void postTransmission() // Функция вызываемая ПОСЛЕ окончания передачи
    {
	if(Modbus_Entered_Critical) {
		xTaskResumeAll();
		Modbus_Entered_Critical = 0;
	}
    #ifdef PIN_MODBUS_RSE
	#if MODBUS_TIME_TRANSMISION != 0
    _delay(MODBUS_TIME_TRANSMISION);// Минимальная пауза между командой и ответом 3.5 символа
	#endif
    digitalWriteDirect(PIN_MODBUS_RSE, LOW);
    #endif
}

// Инициализация Modbus без проверки связи связи
int8_t devModbus::initModbus()    
     {
#ifdef MODBUS_PORT_NUM
        flags=0x00;
        SETBIT1(flags,fModbus);                                                      // модбас присутствует
	#ifdef PIN_MODBUS_RSE
        pinMode(PIN_MODBUS_RSE , OUTPUT);                                            // Подготовка управлением полудуплексом
        digitalWriteDirect(PIN_MODBUS_RSE , LOW);
	#endif
        MODBUS_PORT_NUM.begin(MODBUS_PORT_SPEED,MODBUS_PORT_CONFIG);                 // SERIAL_8N1 - настройки по умолчанию
        RS485.begin(1,MODBUS_PORT_NUM);                                              // Привязать к сериал
        // Назначение функций обратного вызова
        RS485.preTransmission(preTransmission);
        RS485.postTransmission(postTransmission);
        RS485.idle(idle);
        err=OK;                                                                      // Связь есть
#else
        flags=0x00;
        SETBIT0(flags,fModbus);                                                     // модбас отсутвует
        err=ERR_NO_MODBUS;
#endif
        return err;                                                                 
     }
     
// ФУНКЦИИ ЧТЕНИЯ ----------------------------------------------------------------------------------------------
// Получить значение 2-x (Modbus function 0x04 Read Input Registers) регистров (4 байта) в виде float возвращает код ошибки данные кладутся в ret
int8_t devModbus::readInputRegistersFloat(uint8_t id, uint16_t cmd, float *ret)
{
	// Если шедулер запущен то захватываем семафор
	if(SemaphoreTake(xModbusSemaphore, (MODBUS_TIME_WAIT / portTICK_PERIOD_MS)) == pdFALSE) // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
	{
		journal.jprintf((char*) cErrorMutex, __FUNCTION__, MutexModbusBuzy);
		return err = ERR_485_BUZY;
	}
	RS485.set_slave(id);
	uint8_t result = RS485.readInputRegisters(cmd, 2);                                               // послать запрос,
	if(result == RS485.ku8MBSuccess) {
		err = OK;
		*ret = fromInt16ToFloat(RS485.getResponseBuffer(0), RS485.getResponseBuffer(1));
		SemaphoreGive(xModbusSemaphore);
		return OK;
	} else {
		*ret = 0;
		SemaphoreGive(xModbusSemaphore);
		return err = translateErr(result);
	}
}

// Получить значение регистра (2 байта) в виде целого  числа возвращает код ошибки данные кладутся в ret
int8_t devModbus::readHoldingRegisters16(uint8_t id, uint16_t cmd, uint16_t *ret)
{
	// Если шедулер запущен то захватываем семафор
	if(SemaphoreTake(xModbusSemaphore, (MODBUS_TIME_WAIT / portTICK_PERIOD_MS)) == pdFALSE) // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
	{
		journal.jprintf((char*) cErrorMutex, __FUNCTION__, MutexModbusBuzy);
		return err = ERR_485_BUZY;
	}
	RS485.set_slave(id);
	uint8_t result = RS485.readHoldingRegisters(cmd, 1);                                                   // послать запрос,
	if(result == RS485.ku8MBSuccess) {
		*ret = RS485.getResponseBuffer(0);
		err = OK;
	} else {
		*ret = 0;
		err = translateErr(result);
	}
	SemaphoreGive(xModbusSemaphore);
	return err;
}
    
// Получить значение 2-x регистров (4 байта) в виде целого  числа возвращает код ошибки данные кладутся в ret
int8_t devModbus::readHoldingRegisters32(uint8_t id, uint16_t cmd, uint32_t *ret)
{
	// Если шедулер запущен то захватываем семафор
	if(SemaphoreTake(xModbusSemaphore, (MODBUS_TIME_WAIT / portTICK_PERIOD_MS)) == pdFALSE) // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
	{
		journal.jprintf((char*) cErrorMutex, __FUNCTION__, MutexModbusBuzy);
		return err = ERR_485_BUZY;
	}
	RS485.set_slave(id);
	uint8_t result = RS485.readHoldingRegisters(cmd, 2);                                             // послать запрос,
	if(result == RS485.ku8MBSuccess) {
		*ret = (RS485.getResponseBuffer(0) << 16) | RS485.getResponseBuffer(1);
		err = OK;
	} else {
		*ret = 0;
		err = translateErr(result);
	}
	SemaphoreGive(xModbusSemaphore);
	return err;
}
      
// Получить значение 2-x регистров (4 байта) в виде float возвращает код ошибки данные кладутся в ret
int8_t devModbus::readHoldingRegistersFloat(uint8_t id, uint16_t cmd, float *ret)
{
	// Если шедулер запущен то захватываем семафор
	if(SemaphoreTake(xModbusSemaphore, (MODBUS_TIME_WAIT / portTICK_PERIOD_MS)) == pdFALSE)      // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
	{
		journal.jprintf((char*) cErrorMutex, __FUNCTION__, MutexModbusBuzy);
		return err = ERR_485_BUZY;
	}
	RS485.set_slave(id);
	uint8_t result = RS485.readHoldingRegisters(cmd, 2);                                             // послать запрос,
	if(result == RS485.ku8MBSuccess) {
		err = OK;
		*ret = fromInt16ToFloat(RS485.getResponseBuffer(0), RS485.getResponseBuffer(1));
	} else {
		err = translateErr(result);
		*ret = 0;
	}
	SemaphoreGive (xModbusSemaphore);
	return err;
}


// Получить значение N регистров c cmd (2*N байта) МХ2 в виде целого  числа (uint16_t *buf) при ошибке возвращает err
int8_t devModbus::readHoldingRegistersNN(uint8_t id,uint16_t cmd, uint16_t num, uint16_t *buf) 
{
    // Если шедулер запущен то захватываем семафор
    if(SemaphoreTake(xModbusSemaphore,(MODBUS_TIME_WAIT/portTICK_PERIOD_MS))==pdFALSE)     // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
    { journal.jprintf((char*)cErrorMutex,__FUNCTION__,MutexModbusBuzy);return err = ERR_485_BUZY;}
      RS485.set_slave(id);
      uint8_t result = RS485.readHoldingRegisters(cmd,num);                                           // послать запрос,
      if (result == RS485.ku8MBSuccess) 
      { 
        for (int16_t i=0;i<num;i++)   buf[i]=RS485.getResponseBuffer(i);
        err=OK; 
        SemaphoreGive(xModbusSemaphore);
        return err; 
       }  
      else {err=translateErr(result); SemaphoreGive(xModbusSemaphore); return err;}
}

// прочитать отдельный бит возвращает ошибку Modbus function 0x01 Read Coils.
int8_t  devModbus::readCoil(uint8_t id,uint16_t cmd, boolean *ret)                    
{
uint8_t result;
// Если шедулер запущен то захватываем семафор
if(SemaphoreTake(xModbusSemaphore,(MODBUS_TIME_WAIT/portTICK_PERIOD_MS))==pdFALSE)            // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
{ journal.jprintf((char*)cErrorMutex,__FUNCTION__,MutexModbusBuzy);return err = ERR_485_BUZY;}
  RS485.set_slave(id); 
  result = RS485.readCoils(cmd,1);                                                              // послать запрос, Нумерация регистров с НУЛЯ!!!!
  if (result == RS485.ku8MBSuccess) { err=OK; SemaphoreGive(xModbusSemaphore); if (RS485.getResponseBuffer(0)) *ret=true; else *ret=false; return err;}
  else                              { err=translateErr(result); SemaphoreGive(xModbusSemaphore); return err;}
  }


// ФУНКЦИИ ЗАПИСИ ----------------------------------------------------------------------------------------------
// установить битовый вход функция Modbus function 0x05 Write Single Coil.
int8_t devModbus::writeSingleCoil(uint8_t id,uint16_t cmd, uint8_t u8State)
{
    uint8_t result;
    // Если шедулер запущен то захватываем семафор
    if(SemaphoreTake(xModbusSemaphore,(MODBUS_TIME_WAIT/portTICK_PERIOD_MS))==pdFALSE)     // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
    { journal.jprintf((char*)cErrorMutex,__FUNCTION__,MutexModbusBuzy);return err = ERR_485_BUZY;}
      RS485.set_slave(id);
      result = RS485.writeSingleCoil(cmd,u8State);                                         // послать запрос,
      if (result == RS485.ku8MBSuccess) 
      { 
        err=OK; 
        SemaphoreGive(xModbusSemaphore);
        return err; 
       }  
      else {err=translateErr(result); SemaphoreGive(xModbusSemaphore); return err;}
}
// Установить значение регистра (2 байта) МХ2 в виде целого  числа возвращает код ошибки данные data
int8_t   devModbus::writeHoldingRegisters16(uint8_t id, uint16_t cmd, uint16_t data)
{
    // Если шедулер запущен то захватываем семафор
    if(SemaphoreTake(xModbusSemaphore,(MODBUS_TIME_WAIT/portTICK_PERIOD_MS))==pdFALSE)            // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
    { journal.jprintf((char*)cErrorMutex,__FUNCTION__,MutexModbusBuzy); return err = ERR_485_BUZY;}

      RS485.set_slave(id);
      uint8_t result = RS485.writeSingleRegister(cmd,data);                                               // послать запрос,
      SemaphoreGive(xModbusSemaphore);
      return err = translateErr(result);
  
}

// Записать 2 регистра подряд возвращает код ошибки
int8_t devModbus::writeHoldingRegisters32(uint8_t id, uint16_t cmd, uint32_t data)
{
   uint8_t result;
    // Если шедулер запущен то захватываем семафор
    if(SemaphoreTake(xModbusSemaphore,(MODBUS_TIME_WAIT/portTICK_PERIOD_MS))==pdFALSE)            // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
    { journal.jprintf((char*)cErrorMutex,__FUNCTION__,MutexModbusBuzy);return err = ERR_485_BUZY;}
      RS485.set_slave(id);
      RS485.setTransmitBuffer(0, data >> 16);
      RS485.setTransmitBuffer(1, data & 0xFFFF);
      result = RS485.writeMultipleRegisters(cmd,2);                                                 // послать запрос,
      SemaphoreGive(xModbusSemaphore);
      return err = translateErr(result);
}

// Записать float как 2 регистра числа возвращает код ошибки данные dat
int8_t   devModbus::writeHoldingRegistersFloat(uint8_t id, uint16_t cmd, float dat)
{
  union  {
          float f;
          uint16_t i[2];
         } float_map = { .f = dat }; 
    uint8_t result;
    // Если шедулер запущен то захватываем семафор
    if(SemaphoreTake(xModbusSemaphore,(MODBUS_TIME_WAIT/portTICK_PERIOD_MS))==pdFALSE)            // Захват мютекса потока или ОЖИДАНИНЕ MODBUS_TIME_WAIT
    { journal.jprintf((char*)cErrorMutex,__FUNCTION__,MutexModbusBuzy);return err = ERR_485_BUZY;}

      RS485.set_slave(id);
      RS485.setTransmitBuffer(0,float_map.i[1]);
      RS485.setTransmitBuffer(1,float_map.i[0]);
     result = RS485.writeMultipleRegisters(cmd,2);
   //   result = RS485.writeSingleRegister(cmd,dat);                                               // послать запрос,
       SemaphoreGive(xModbusSemaphore);
      return err = translateErr(result);
  
}

// Тестирование связи возвращает код ошибки
#ifndef FC_VACON
int8_t devModbus::LinkTestOmronMX2()
{
  uint16_t result, ret;
  err=OK;
  RS485.set_slave(FC_MODBUS_ADR);
  result = RS485.LinkTestOmronMX2Only(TEST_NUMBER);                                              // Послать команду проверки связи
  if (result == RS485.ku8MBSuccess) ret=RS485.getResponseBuffer(0);                              // Получить данные с ответа
  else return err=ERR_485_INIT;                                                                  // Ошибка инициализации
  if (TEST_NUMBER!=ret) return err=ERR_MODBUS_MX2_0x05;  // Контрольные данные не совпали
  return err;                                                    
}
#endif
// Перевод ошибки протокола Модбас (что дает либа) в ошибки ТН, учитывается спицифика Инверторов
// коды ошибок у инверторов могут отличаться
int8_t devModbus::translateErr(uint8_t result)
{
 switch (result)
    {
    // Сдандартные ошибки протокола modbus  едины для всех устройств на модбасе
    case 0x00:      return OK;                  break;  
    case 0x01:      return ERR_MODBUS_0x01;     break;
    case 0x02:      return ERR_MODBUS_0x02;     break;
    case 0x03:      return ERR_MODBUS_0x03;     break;
    case 0x04:      return ERR_MODBUS_0x04;     break;
    case 0xe0:      return ERR_MODBUS_0xe0;     break;
    case 0xe1:      return ERR_MODBUS_0xe1;     break;
    case 0xe2:      return ERR_MODBUS_0xe2;     break;
    case 0xe3:      return ERR_MODBUS_0xe3;     break;    
    #ifdef FC_VACON
      case 0x05:      return ERR_MODBUS_VACON_0x05;break;
      case 0x06:      return ERR_MODBUS_VACON_0x06;break;
      case 0x07:      return ERR_MODBUS_VACON_0x07;break;
      case 0x08:      return ERR_MODBUS_VACON_0x08;break;
    #else
      case 0x05:      return ERR_MODBUS_MX2_0x05; break;
      case 0x08+0x01: return ERR_MODBUS_MX2_0x01; break;
      case 0x08+0x02: return ERR_MODBUS_MX2_0x02; break;
      case 0x08+0x03: return ERR_MODBUS_MX2_0x03; break;
      case 0x08+0x21: return ERR_MODBUS_MX2_0x21; break;
      case 0x08+0x22: return ERR_MODBUS_MX2_0x22; break;
      case 0x08+0x23: return ERR_MODBUS_MX2_0x23; break;
    #endif
    default  :      return ERR_MODBUS_UNKNOW;   break;
    }
 
}
