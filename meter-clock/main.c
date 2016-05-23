/*
* meter-clock.c
*
* Created: 5/23/2016 3:25:41 PM
* Author : Steven
*/

#define F_CPU 1000000UL
#include <avr/io.h>
#include <util/delay.h>
#include "debounce.h"
#include "i2cmaster.h"

//Masks for DS1307 RTC data
#define RTC_ADDR 0xD0
#define RTC_10SEC_MASK 0x70
#define RTC_SEC_MASK 0x0F
#define RTC_10MIN_MASK 0x70
#define RTC_MIN_MASK 0x0F
#define RTC_AM_MASK 0x20
#define RTC_10HR_MASK 0x10
#define RTC_HR_MASK 0x0F
#define RTC_DOW_MASK 0x03
#define RTC_10DATE_MASK 0x30
#define RTC_DATE_MASK 0x0F
#define RTC_10MONTH_MASK 0x10
#define RTC_MONTH_MASK 0x0F
#define RTC_10YR_MASK 0xF0
#define RTC_YR_MASK 0x0F
#define HIGH_NIB 0xF0
#define LOW_NIB 0x0F

//Clock has 2 led indicators
#define LED1_PORT PORTD3
#define LED2_PORT PORTD4

void config_rtc(void);
void read_rtc(void);
void write_rtc(void);
void update_pwm(void);
char bin_to_bcd(char);
void set_leds(char,char);

//holds time data to/from RTC
volatile char sec;
volatile char min;
volatile char hrs;
//volatile char dow;
//volatile char date;
//volatile char month;
//volatile char year;
volatile char ctl;

//Order that clock is set
enum set_order {Meter_Cal, Hour, Min};
volatile enum set_order cur_set = Meter_Cal;

//Clock runs normally in Run mode, but PB goes to set mode
//Set on startup
enum modes {Run, Set};
volatile enum modes cur_mode = Set;

ISR(TIMER2_OVF_vect)
{
	debounce();
}

int main(void)
{
	char btn_cnt = 0;
	
	//Set up led indicators
	DDRD |= _BV(LED1_PORT);
	DDRD |= _BV(LED2_PORT);
	
	//Set up Timer for debounce
	TCCR2B = _BV(CS22);
	TIMSK2 = _BV(TOIE2);
	
	//Set up timer for pwm
	//fast pwm, non inverting. Sets pin oc0b, for hours
	TCCR0A = _BV(COM0B1) | _BV(WGM02) | _BV(WGM00); //com0b = 10, wgm = 111
	TCCR0B = _BV(CS00) | _BV(CS01) | _BV(WGM02);//clock div is 64
	OCR0A = 12; //top of pwm
	OCR0B = 6; //set 50%
	
	//fast pwm, non inverting. oc1a is mins, oc1b is secs.
	ICR1 = 0x003C; //top = 60
	OCR1A = 30; //set both pwm to 50%
	OCR1B = 30;
	TCCR1A = _BV(COM1A1) | _BV(COM1B1) | _BV(WGM11); //com1a = com1b = 10 for non inverting
	TCCR1B = _BV(CS11) | _BV(WGM13);//| _BV(WGM12); //WGM = 1110 for fast pwm with icr as top, CS = 010 so clock div is 8
	
	//Set pwm as outputs
	DDRB |= _BV(PORTB1) | _BV(PORTB2); //pb1 is oc1a is minutes. pb2 is oc1b is seconds
	DDRD |= _BV(PORTD5); //pd5 is oc0b is hours
	
	debounce_init();
	sei();
	
	//Set up SPI
	//B5 - SCK, B4 - MISO, B3 - MOSI, B2 - /SS
	//DDRB |= _BV(DDB5) | _BV(DDB3) | _BV(DDB2);
	//SPCR |= _BV(SPE) | _BV(MSTR);
	
	//Set up I2C
	i2c_init();
	
	//Initialize RTC
	read_rtc();
	config_rtc();
	
	//Initialize LED Driver
	//write_spi(DRV_LIMIT, 0x05);
	//write_spi(DRV_MODE, 0xFF);
	//write_spi(DRV_INTENSITY, intens);
	//write_spi(DRV_ENA, 0x01);
	
	while (1)
	{
		//Process button inputs
		//User wants to set time
		if(button_down(BTNMODE_MASK))
		{
			switch (cur_mode)
			{
				case Run: cur_mode = Set; break;
				case Set:
				if(cur_set < Min)
				{
					cur_set++;
					btn_cnt = 0;
				}
				else
				{
					cur_mode = Run;
					cur_set = Meter_Cal;
					btn_cnt = 0;
					write_rtc();
				}
				break;
			}
		}
		
		//User increments value
		if(button_down(BTNINC_MASK))
		{
			if (cur_mode == Set && btn_cnt < 60)
			{
				btn_cnt++;
			}
		}
		
		//User decrements value
		if(button_down(BTNDEC_MASK))
		{
			if (cur_mode == Set && btn_cnt > 0)
			{
				btn_cnt--;
			}
		}
		
		//Display or set the time
		if (cur_mode == Run)
		{
			read_rtc();
			update_pwm();
			if ((sec & 1) == 1)
			{
				set_leds(1,0);
			}
			else
			{
				set_leds(0,1);
			}
		}
		else
		{
			switch (cur_set)
			{
				case Meter_Cal:
				//Set all meters to 50% for pot adjustment
				OCR0B = 6;
				OCR1A = 30;
				OCR1B = 30;
				set_leds(1,1);
				break;
				
				case Hour:
				//Set hour of clock
				if (btn_cnt > 12) btn_cnt = 12;
				hrs = bin_to_bcd(btn_cnt) & (RTC_10HR_MASK | RTC_HR_MASK);
				hrs |= 0b01000000; //12/24 is high
				update_pwm();
				set_leds(1,0);
				break;
				
				case Min:
				//Set minutes of clock
				if (btn_cnt > 59) btn_cnt = 59;
				min = bin_to_bcd(btn_cnt) & (RTC_10MIN_MASK | RTC_MIN_MASK);
				update_pwm();
				set_leds(0,1);
				break;
			}
			sec = 0;
		}
	}
}

//Initialize RTC: no square wave out, ena oscillator
void config_rtc(void)
{
	i2c_start_wait(RTC_ADDR + I2C_WRITE);
	i2c_write(0x07); //set ctrl reg
	i2c_write(0x10); //turn off sq wave
	i2c_stop();
	
	i2c_start_wait(RTC_ADDR + I2C_WRITE);
	i2c_write(0x00); //set sec reg
	i2c_write(sec & 0x7F); //turn on osc
	i2c_stop();
}

void write_rtc(void)
{
	i2c_start_wait(RTC_ADDR + I2C_WRITE);
	i2c_write(0x00);
	i2c_write(sec);
	i2c_write(min);
	i2c_write(hrs);
	//i2c_write(dow);
	//i2c_write(date);
	//i2c_write(month);
	//i2c_write(year);
	i2c_stop();
}

void read_rtc(void)
{
	i2c_start_wait(RTC_ADDR + I2C_WRITE);
	i2c_write(0x00);
	i2c_rep_start(RTC_ADDR + I2C_READ);
	sec = i2c_readAck();
	min = i2c_readAck();
	hrs = i2c_readNak();
	//dow = i2c_readAck();
	//date = i2c_readAck();
	//month = i2c_readAck();
	//year = i2c_readNak();
	i2c_stop();
}

void update_pwm(void)
{
	//Update hours
	OCR0B = (((RTC_10HR_MASK & hrs) >> 4) * 10) + (RTC_HR_MASK & hrs);
	
	//Update minutes
	OCR1A = (((RTC_10MIN_MASK & min) >> 4) * 10) + (RTC_MIN_MASK & min);
	
	//Update seconds
	OCR1B = (((RTC_10SEC_MASK & sec) >> 4) * 10) + (RTC_SEC_MASK & sec);
}

//RTC requires bcd encoding
char bin_to_bcd(char in)
{
	char tens = 0;
	while(in >= 10)
	{
		tens++;
		in -=10;
	}
	
	return (tens << 4) | in;
}

void set_leds(char led1, char led2)
{
	if (led1)
	{
		PORTD |= _BV(LED1_PORT);
	}
	else
	{
		PORTD &= ~_BV(LED1_PORT);
	}
	
	if (led2)
	{
		PORTD |= _BV(LED2_PORT);
	}
	else
	{
		PORTD &= ~_BV(LED2_PORT);
	}
}