/*
 * Copyright 2009-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*
 * This "boot applet" assists the boot ROM by initializing the hardware platform
 * to facilitate and optimize booting. For example, it initializes the external
 * memory controller, so applications can be loaded directly to external SDRAM.
 * This work is not done in the ROM, because it is platform dependent.
 */
//
////////////////////////////////////////////////////////////////////////////////
#include "types.h"
#include "error.h"

#include "registers/regsrtc.h"
#include "registers/regspower.h"
#include "registers/regsdigctl.h"
#include "registers/regsicoll.h"
#include "registers/hw_irq.h"
#include "hw/power/hw_power.h"
#include "hw/icoll/hw_icoll.h"
#include "hw/core/hw_core.h"
#include "hw/lradc/hw_lradc.h"
#include "hw/digctl/hw_digctl.h"
#include "drivers/power/ddi_power.h"
#include "registers/regsuartdbg.h"
#include "registers/regsclkctrl.h"
#include "registers/regslradc.h"
#include <stdarg.h>
#include "debug.h"

/* for debugging and testing */
#include "registers/regspinctrl.h"
////////////////////////////////////////////////////////////////////////////////
// Prototypes
////////////////////////////////////////////////////////////////////////////////
bool IsVbusValid( void );
bool IsVdd5vGtVddio( void );
bool Is5vPresent( void );
bool IsBattLevelValidForBoot( void );
void PowerPrep_ClearAutoRestart( void );
int PowerPrep_ConfigurePowerSource( void );
void PowerPrep_PowerDown( void );
void PowerPrep_Setup5vDetect( void );
void PowerPrep_SetupBattDetect( void );
int PowerPrep_BootValid5v( void );
void PowerPrep_Enable4p2( void );
void PowerPrep_Init4p2Parameters( void );
void PowerPrep_Init4p2Regulator( void );
void PowerPrep_Enable4p2BoShutdown( void );
void PowerPrep_Handle5vDisconnectErrata( void );
void PowerPrep_Enable4p2Fiq( void );
void PowerPrep_InitDcdc4p2Source( void );
int PowerPrep_Handle5vConflict( void );
int PowerPrep_InitBattBoFiq( void );
int PowerPrep_InitBattBo( void );
int PowerPrep_Init4p2Bo( void );
int PowerPrep_Init4p2BoFiq( void );
int PowerPrep_5vBoot( void );
int PowerPrep_BattBoot( void );
void PowerPrep_SwitchVdddToDdcdcSource( void );
void PowerPrep_EnableOutputRailProtection( void );
bool PowerPrep_IsBatteryReady( void );
void PowerPrep_StopCharger();
void PowerPrep_SetCharger(unsigned int current);
bool PowerPrep_IsBatteryGood();
void PowerPrep_CPUClock2XTAL(void);
void PowerPrep_PrintBatteryVoltage(unsigned int value);
////////////////////////////////////////////////////////////////////////////////
// Definitions
////////////////////////////////////////////////////////////////////////////////
#define POWER_PREP_GROUP                        (0x80000000)
#define ERROR_VBUSVALID_RACE_CONDITION          ((int)POWER_PREP_GROUP + 1)
#define ERROR_VDDIO_BROWNOUT                    ((int)POWER_PREP_GROUP + 2)
#define ERROR_5V_OSCILLATING                    ((int)POWER_PREP_GROUP + 3)
#define ERROR_INSUFFICIENT_BATT_AFTER_PSWITCH   ((int)POWER_PREP_GROUP + 4)
#define ERROR_UNEXPECTED_LOOP_EXIT              ((int)POWER_PREP_GROUP + 5)


/* We need to protect the DCDC from trying to source charge
 * from too low of a voltage.  This limit is given in
 * the characteristics and specifications section of the
 * reference manual.  Also, we want to stop sourcing
 * charge from the battery before the battery's internal
 * protection circuitry may activate which would then cause
 * us to lose RTC time.  Generically using 3.0V as the
 * battery's maximum voltage it may activate it's
 * undervoltage protection.  At the time of this writing,
 * 3V is greater than the minimum DCDC operating voltage
 * so we will set the BRWNOUT_LVL bitfield to 3V value.
 */
#define BATTERY_BRWNOUT_BITFIELD_VALUE 15 /* 15 = 3.0V */

/* per the reference manul, the power supplies DCDC output capabilities
 * are given with 3.3V as the minimum battery input voltage. By default,
 * we'd rather not start the device unless we have know that we can
 * supply a known amount of power (given in the reference manual).
 * This value is used only for booting from battery, not from 5V.
 * Typically, you'll want to set this slightly above the lowest
 * input voltage you want to provide the DCDC during boot to account
 * for larger voltage drops battery output resistance when the load
 * is higher than when this voltage check is made.
 *
 * Lastly, we want this voltage high enough above the battery
 * brownout level so that we don't accidentally trigger a
 * battery brownout later on during a more critical time
 * (such as when writing to media which might cause some
 * media corruption).
 */
#define MINIMUM_SAFE_BOOTING_BATTERY_VOLTAGE_MV 3600


#define VDD4P2_ENABLED


#define MAX_4P2_CAP_CHARGE_TIME 100000
#define BOOTUP_CHARGE_4P2_CURRENT_LIMIT 780
#define POWER_PREP_MINIMUM_VALID_BATTERY_BOOTUP_VOLTAGE 3200
#define MAX_BATTERY_CAPACITOR_SIZE_UF 100

/* Can't limit current by 4P2 because chip may shutdown
   Software check if enable current limition to judge current
   CPU frequency.
*/
#define VBUS_CURRENT_LIMIT 400
#define BATTERY_LOW            2400
#define BATTERY_HIGH           4300
#define BATTERY_BAD            3500
#define BATTERY_BOOT           3400


/* enable the following if your device configuration consists uses a regulated
* voltage source for the battery voltage input.  Source must be in the range
* of the recommended operating voltage range from the characteristics and
* specifications section of the mx23/mx28 reference manual
*/
/* #define NO_BATTERY_VOLTAGE_SOURCE */

#if 0
#define RUN_BATTERY_TEST
#define BATTERY_TEST_CHARGE_CURRENT_MA 50
#define BATTERY_TEST_CHARGE_VOLTAGE_THRESHOLD_MV 3900
#endif

#define DISABLE_VDDIO_BO_PROTECTION
#define NO_BATTERY_VOLTAGE_SOURCE
////////////////////////////////////////////////////////////////////////////////
// Globals
////////////////////////////////////////////////////////////////////////////////
#if 0
bool bBatteryConnected = false;
#endif

bool bBatteryReady = false;
bool bBatteryGood = false;
////////////////////////////////////////////////////////////////////////////////
// Code
////////////////////////////////////////////////////////////////////////////////

bool IsVbusValid( void )
{
    return hw_power_GetVbusValid();
}

bool IsVdd5vGtVddio( void )
{
    return hw_power_GetVdd5vGtVddio();
}

bool Is5vPresent( void )
{
    return ( IsVbusValid() || IsVdd5vGtVddio() );
}

bool IsBattLevelValidForBoot( void )
{
	bool bBattLevelValid=false;

	if((HW_POWER_BATTMONITOR.B.BATT_VAL * 8) >=
		POWER_PREP_MINIMUM_VALID_BATTERY_BOOTUP_VOLTAGE)
	{
		bBattLevelValid=true;
	}

    return (bBattLevelValid);
}


////////////////////////////////////////////////////////////////////////////////
//! \brief  Prepares the power block for the application.
//!
//! \return SUCCESS(0) Power block ready.
////////////////////////////////////////////////////////////////////////////////
//int PowerPrep( void )
int _start( void )
{
	int iRtn = SUCCESS,i;
#ifndef mx28
	HW_DIGCTL_CTRL_SET(BM_DIGCTL_CTRL_USE_SERIAL_JTAG);
#endif

	PowerPrep_CPUClock2XTAL();
	PowerPrep_ClearAutoRestart();

	hw_power_SetPowerClkGate( false );
		i=0;
#ifndef PressDelay	//system start wait time (unit:0.5s)
#define PressDelay 2	//wait 1s
#endif
#if 0
              while(i++ < PressDelay)
			{
				printf(">");
				PowerPrep_Delay(250000); //200000microsec
				printf(">");
				PowerPrep_Delay(250000);
			}
#endif
	PowerPrep_Delay(10000); 
	printf("\r\nPowerPrep start initialize power...\r\n");
	HW_POWER_VDDDCTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;
	HW_POWER_VDDACTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;
	HW_POWER_VDDIOCTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;

	// Ready the power block for 5V detection.
	PowerPrep_Setup5vDetect();
//	PowerPrep_SetupBattDetect();

	// Ensure the power source that turned on the device is sufficient to
	// power the device.
	PowerPrep_ConfigurePowerSource();

	PowerPrep_EnableOutputRailProtection();
	/* 3.3V is necessary to achieve best power supply capability
	* and best EMI I/O performance.
	*/
	ddi_power_SetVddio(3300, 3150);

#ifdef mx28
	ddi_power_SetVddd(1350, 1200);
#endif

	HW_POWER_CTRL_CLR(BM_POWER_CTRL_VDDD_BO_IRQ |
	BM_POWER_CTRL_VDDA_BO_IRQ |
	BM_POWER_CTRL_VDDIO_BO_IRQ |
	BM_POWER_CTRL_VDD5V_DROOP_IRQ |
	BM_POWER_CTRL_VBUSVALID_IRQ |
	BM_POWER_CTRL_BATT_BO_IRQ |
	BM_POWER_CTRL_DCDC4P2_BO_IRQ
	);
	/* If Battery not ready,setup the auto power down if we lost 5V.*/
	if (!bBatteryReady)
		HW_POWER_5VCTRL_SET(BM_POWER_5VCTRL_PWDN_5VBRNOUT);
	printf("\r\ndebug return");	
	return iRtn;
}

/* clear RTC ALARM wakeup or AUTORESTART bits here. */
void PowerPrep_ClearAutoRestart( void )
{
	HW_RTC_CTRL_CLR( BM_ICOLL_CTRL_SFTRST );
	while( HW_RTC_CTRL.B.SFTRST == 1 );
	HW_RTC_CTRL_CLR( BM_ICOLL_CTRL_CLKGATE );
	while( HW_RTC_CTRL.B.CLKGATE == 1 );
        /* Due to the hardware design bug of mx28 EVK-A
        * we need to set the AUTO_RESTART bit.
	*/
	if(HW_RTC_PERSISTENT0.B.AUTO_RESTART==0)
	{
		while(BF_RD( RTC_STAT, NEW_REGS));
		HW_RTC_PERSISTENT0.B.AUTO_RESTART = 1;
		BF_SET(RTC_CTRL, FORCE_UPDATE);
		BF_CLR(RTC_CTRL, FORCE_UPDATE);
		while(BF_RD( RTC_STAT, NEW_REGS));
		while(BF_RD( RTC_STAT, STALE_REGS));
	}
}
////////////////////////////////////////////////////////////////////////////////
//! \brief
//!
//! \return
////////////////////////////////////////////////////////////////////////////////
int PowerPrep_ConfigurePowerSource( void )
{
	int iReturnValue = SUCCESS;

	/* initialize DCDC operating parameters */
	hw_power_Init();

#ifdef  NO_BATTERY_VOLTAGE_SOURCE

	/* device configured for "no battery" operation (5V only power source).  This boot
	* option doesn't waste time looking for a good battery.  Battery powered
	* operation and automatic voltage measurements are disabled.
	*/

	bBatteryReady = false;
	bBatteryGood = false;

	printf("\r\nConfigured for 5v only power source.\
		Battery powered operation disabled.\r\n");

	/* disable automatic battery voltage measurements */
	BF_CLR(LRADC_CONVERSION, AUTOMATIC);
	BF_WR(POWER_BATTMONITOR, BATT_VAL,0);

	iReturnValue = PowerPrep_5vBoot();

#ifndef MXS_VBUS_CURRENT_DRAW
	PowerPrep_CPUClock2PLL();
#endif
#else
	/* check if Battery Voltage is high enough for full
	* power operation.
	*/
	bBatteryReady = PowerPrep_IsBatteryReady();

	if(!IsVdd5vGtVddio())
	{
		/* this option allows for fast booting without
		* the unecessary delay required for proper
		* battery detection when booting from 5v.
		*/

		/* battery voltage is present and 5v is not connected,
		* assume valid battery voltage source is present.  Battery
		* brownout protection will take care of supply protection
		* if battery becomes invalid in the future
		*/
 		bBatteryGood = true;
		printf("\r\nboot from battery. 5v input not detected\r\n");
		/*Boot from battery*/
		iReturnValue = PowerPrep_BattBoot();
		PowerPrep_CPUClock2PLL();
	}
	else
	{
		/* 5v input detected.  Now we need to determine if a
		* valid battery source is present to decide which
		* power source to use during bootup.
		*/
		bBatteryGood = PowerPrep_IsBatteryGood();
		if(!bBatteryReady)
		{
			if(bBatteryGood)
				/* control arrives here if the battery voltage source
				* appears to be chargeable but the voltage is current
				* too low to use as a reliable battery voltage source.
				*/
				printf("\r\nChargeable battery detected but\
					the voltage is too low for battery\
					powered operation.Booting from 5V\
					power source.\r\n");
			else {
				BF_CLR(LRADC_CONVERSION, AUTOMATIC);
				BF_WR(POWER_BATTMONITOR, BATT_VAL,0);
				printf("\r\nNo battery or bad battery\
					detected!!!.Disabling battery\
					voltage measurements./r/n");
			}
			iReturnValue = PowerPrep_5vBoot();
#ifndef MXS_VBUS_CURRENT_DRAW
			PowerPrep_CPUClock2PLL();
#endif /* MXS_VBUS_CURRENT_DRAW */
		}
		else
		{
			printf("\r\n5v source detected.Valid battery\
				voltage detected.Booting from battery\
				voltage source.\r\n");
			/*Boot from battery*/
			iReturnValue = PowerPrep_BattBoot();
			PowerPrep_CPUClock2PLL();

		}
	}
#endif
	/* raise battery brownout level to programmed value. */
	PowerPrep_InitBattBo();

	/* Lastly, let's switch the Vddd power source to DCDC instead of
	* the linear regulator(linear regulator is the hardware default
	* configuration at bootup.
	*/
	PowerPrep_SwitchVdddToDdcdcSource();

	return iReturnValue;
}


void PowerPrep_PowerDown( void )
{
#if 0
	/* If chip is powering down unexpectedly, add this code
	*/ in to catch the chip shutdown before it occurs.
	volatile int Wait = 1;
	while(Wait);
	#endif

	hw_power_PowerDown();
}

void PowerPrep_Setup5vDetect( void )
{
    //---------------------------------------------------------------------------
    // Turn on the 5V detection hardware.
    //---------------------------------------------------------------------------
    hw_power_EnableVbusValid5vDetect( true );


    //---------------------------------------------------------------------------
    // Select the 5V detection methods and thresholds based on the chip.
    //---------------------------------------------------------------------------
    // 378x can use VBUSVALID for all 5V detection.  Use the normal threshold.
    hw_power_SetVbusValidThresh( VBUS_VALID_THRESH_4400 );



}

void PowerPrep_SetupBattDetect( void )
{
    extern bool bHwLradcInitialized;

    // Initialize LRADC for battery monitoring.
    bHwLradcInitialized = false;
    hw_lradc_Init(FALSE, LRADC_CLOCK_6MHZ);

    hw_lradc_EnableBatteryMeasurement( hw_lradc_AcquireBatteryMonitorDelayTrigger(),
                                       100);
}

#if 0
bool PowerPrep_BatteryDischargeTest( void )
{
	uint16_t starting_voltage = hw_power_GetBatteryVoltage();
	uint32_t counter, loaded_voltage;
	bool bTimedOut = false;


	HW_POWER_CHARGE.B.ENABLE_LOAD = 1;

	counter = hw_digctl_GetCurrentTime();

	/* with a load on 4p2 (and thus load on
	* Vbat via the required external Schottky diode, check
	* to see if voltage drops by >100mV in 10ms.  If so,
	* we think that the battery connection is unreliable.
	* The datasheet says that the ENABLE_LOAD causes a 100ohm
	* load but one test showed a load ~450ohm.  Assuming a
	* maximum resistance of 1kohm for now.
	*/
	while(!bTimedOut)
	{

		bTimedOut = hw_digctl_CheckTimeOut(counter, 10000);

		loaded_voltage = hw_power_GetBatteryVoltage();
		if( ( loaded_voltage => starting voltage ) ||
			(loaded_voltage - starting_voltage < 100) )
		{
			continue;
		}
		else
			exit;
	}

	return (bTimedOut);
}


void PowerPrep_BatteryConnectionChargeTest( void )
{



}
#endif

int PowerPrep_BootValid5v( void )
{

	/* now use VBUSVALID level instead of VDD5V_GT_VDDIO level
	* to trigger a 5V disconnect event
	*/
	HW_POWER_5VCTRL_SET(BM_POWER_5VCTRL_VBUSVALID_5VDETECT);

	/* configure polarity to check for 5V disconnection.  This will
	* give us indication if a VBUSVALID_IRQ event occurred occurred
	* causing 4P2 rail to be disabled in hardware.  The kernel
	* driver can then check the VBUSVALID_IRQ along with the DCDC4P2
	* bit to know whether or not 4P2 is enabled
	*/
	HW_POWER_CTRL_CLR(BM_POWER_CTRL_POLARITY_VBUSVALID |
		BM_POWER_CTRL_POLARITY_VDD5V_GT_VDDIO );

	HW_POWER_CTRL_CLR(BM_POWER_CTRL_VBUSVALID_IRQ |
		BM_POWER_CTRL_VDD5V_GT_VDDIO_IRQ );

#if 0
	/* if battery voltage is so high that we can't reliably
	* run a battery charging test to determine if the battery is
	* connected or not, we have to run a dis-charge test.
	*/
	if(hw_power_GetBatteryVoltage() > 3300)
	{
		bBatteryConnected = PowerPrep_BatteryConnectionDischargeTest();
		bBatteryConnectionTested = true;
	}
#endif
	PowerPrep_Enable4p2();

#if 0
	if(!bBatteryConnectionTested)
	{
		bBatteryConnected = PowerPrep_BatteryConnectionChargeTest();
		bBatteryConnectionTested = true;
	}
#endif


	return SUCCESS;

}


void PowerPrep_CheckBatteryConnection( void )
{



}

void PowerPrep_Enable4p2( void )
{

	uint32_t vdddctrl, vddactrl, vddioctrl;

	/* Disabling of FETs is necessary for the following functionality to
	* enable 4P2 safely when the glitch from mx23 chip errata 5837
	* occurs.  When the glitch occurs and 4P2 rail is restarted,
	* if the DCDC is active and sourced from 4P2, and if the battery is very low
	* or dead, the re-enabling of the 4P2 source causes the DCDC's source
	* to spike up quickly, potentially causing voltage spikes on the output
	* rails which we want to avoid.  A similar but slightly different scenario
	* caused chip damage in previous generations of parts.
	*
	* Also, we the only entrance to this function should be from a 5V connected
	* bootup and the PWDN_5VBRNOUT functionaltiy should be enabled.  We
	* are about to disable all the fets so if we were relying on battery powered
	* DCDC operation and arrive at this point, the device will fail.
	*/

	vdddctrl = HW_POWER_VDDDCTRL_RD();
	vddactrl = HW_POWER_VDDACTRL_RD();
	vddioctrl = HW_POWER_VDDIOCTRL_RD();

	HW_POWER_VDDDCTRL_SET(BM_POWER_VDDDCTRL_DISABLE_FET |
		BM_POWER_VDDDCTRL_ENABLE_LINREG |
		BM_POWER_VDDDCTRL_PWDN_BRNOUT);

	HW_POWER_VDDACTRL_SET(BM_POWER_VDDACTRL_DISABLE_FET |
		BM_POWER_VDDDCTRL_ENABLE_LINREG |
		BM_POWER_VDDDCTRL_PWDN_BRNOUT);

	HW_POWER_VDDIOCTRL_SET(BM_POWER_VDDIOCTRL_DISABLE_FET |
		BM_POWER_VDDDCTRL_PWDN_BRNOUT);

	PowerPrep_Init4p2Parameters();

	PowerPrep_Init4p2Regulator();

	/* if battery isn't ready, go ahead and shutdown on a 4p2 brownout
	* (because hardware battery brownout is disabled if 5V is present
	*/



	if(!bBatteryReady)
		PowerPrep_Enable4p2BoShutdown();
	else
	{
		/* in the case where 4p2 browns out, battery needs to
		* take over powering the system.  We need to ensure
		* the battery voltage level is sufficient to do so.
		* Otherwise, the DCDC will keep sourcing from battery
		* and 4p2 down to level below the safe minimum
		* operating level of the DCDC.
		*/
		PowerPrep_InitBattBoFiq();
	}

	PowerPrep_InitDcdc4p2Source();

	HW_POWER_VDDDCTRL_WR(vdddctrl);
	/* before enabling vdda fet, give some small delay to allow DCDC to handle new load
	* if vddd fet is getting enabled */
	hw_digctl_MicrosecondWait(20);
	HW_POWER_VDDACTRL_WR(vddactrl);
	/* before enabling vddio fet, give some small delay to allow DCDC to handle new load
	* if vdda fet is getting enabled */
	hw_digctl_MicrosecondWait(20);
	HW_POWER_VDDIOCTRL_WR(vddioctrl);

	if( (HW_POWER_VDDDCTRL.B.DISABLE_FET == 0) ||
		(HW_POWER_VDDACTRL.B.DISABLE_FET == 0) ||
		(HW_POWER_VDDIOCTRL.B.DISABLE_FET == 0))
		/* now that DCDC load is present, we can remove the 4p2 internal load */
		BF_CLR(POWER_CHARGE,ENABLE_LOAD);

}

void PowerPrep_Init4p2Parameters( void )
{
	HW_POWER_DCDC4P2.B.CMPTRIP = 31;

	HW_POWER_DCDC4P2.B.TRG = 0;
	HW_POWER_5VCTRL.B.HEADROOM_ADJ = 0x4;
	HW_POWER_DCDC4P2.B.DROPOUT_CTRL = 0xA;  //100mV drop before stealing
		// charging current
	HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT = 0x3f;
}


void PowerPrep_Init4p2Regulator( void )
{

	// Enables but DCDC 4P2 logic.  This appears to be a necessary step not
	// only for DCDC operation from 4p2, but for the 4p2 rail itself to
	// start up properly.
	BF_SET(POWER_DCDC4P2, ENABLE_4P2);  //enable 5V to 4p2 regulator

	// This must be used if there is no static load on 4p2 as 4p2 will
	// become unstable with no load.
	BF_SET(POWER_CHARGE,ENABLE_LOAD);

	/* Initialize 4p2 current limt before powering up 4p2.*/
	HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT = 0;

	HW_POWER_DCDC4P2.B.TRG = 0;

	/*power up the 4p2 rail and logic/control.*/
	hw_power_EnableMaster4p2( true );

	/* Start charging up the 4p2 capacitor.  We ramp of this charge gradually
	* to avoid large inrush current from the 5V cable which can cause
	* transients/problems
	*/

	hw_power_Enable4p2DcdcInput(true);

	if(HW_POWER_CTRL_RD() & BM_POWER_CTRL_VBUSVALID_IRQ)
	{
		/* if we arrived here, we were unable to recover
		* from mx23 chip errata 5837.	4P2 is disabled and sufficient
		* battery power is not present.  Exiting to not enable DCDC
		* power during 5V connected state.
		*/
		hw_power_Enable4p2DcdcInput(false);
		hw_power_EnableMaster4p2( false );

		printf("Enabling of DCDC failed at setting of \
			DCDC4P2 ENABLE_DCDC.  Only 5V power supply \
			operating is the linear regulators.\n");
		return;
	}



	/* here we set the 4p2 brownout level to something very
	* close to 4.2V.  We then check the brownout status.  If the brownout
	* status is false, the voltage is already close to the target
	* voltage of 4.2V so we can go ahead and set the 4P2 current
	* limit to our max target limit.  If the brownout status is true,
	* we need to ramp us the current limit so that we don't cause
	* large inrush current issues.  We step up the current limit until
	* the brownout status is false or until we've reached our maximum
	* defined 4p2 current limit.
	*/
	HW_POWER_DCDC4P2.B.BO = 22; // 4.15V


	if(HW_POWER_STS.B.DCDC_4P2_BO == 0)
	{
		HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT =
			hw_power_ConvertCurrentToSetting(
			BOOTUP_CHARGE_4P2_CURRENT_LIMIT);
	}
	else
	{

		int i = HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT;

		while(i < hw_power_ConvertCurrentToSetting(
				BOOTUP_CHARGE_4P2_CURRENT_LIMIT))

		{
			if(HW_POWER_STS.B.DCDC_4P2_BO == 0)
			{
				i = hw_power_ConvertCurrentToSetting(
				BOOTUP_CHARGE_4P2_CURRENT_LIMIT);
				hw_digctl_MicrosecondWait(100);
				HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT = i;
			}
			else
			{
				i++;
				HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT = i;
				hw_digctl_MicrosecondWait(100);
			}
		}
	}


#ifdef MXS_VBUS_CURRENT_DRAW
		printf("VDD5V is sourced from VBUS,limit the current to 100mA\r\n");
		HW_POWER_5VCTRL.B.CHARGE_4P2_ILIMIT =
			hw_power_ConvertCurrentToSetting(VBUS_CURRENT_LIMIT);

		hw_digctl_MicrosecondWait(10000);
#endif

	HW_POWER_DCDC4P2.B.BO = 0;
	HW_POWER_CTRL_CLR(BM_POWER_CTRL_DCDC4P2_BO_IRQ);

}


/* This function will enable the 4p2 brownout to cause an FIQ.  The default
 * fiq handler in ROM will then cause a shutdown.
 */
void PowerPrep_Enable4p2BoShutdown( void )
{
	/* setting brownout to lowest value.  3.6V is plenty
	 * to power the system.  Going below there means there
	 * is a major problem with the 4p2 rail and so we need
	 * to shut down.
	 */
	HW_POWER_DCDC4P2.B.BO = 0;
	HW_POWER_CTRL_CLR(BM_POWER_CTRL_DCDC4P2_BO_IRQ);
	HW_POWER_CTRL_SET(BM_POWER_CTRL_ENIRQ_DCDC4P2_BO);

	PowerPrep_Enable4p2Fiq();
}

/* See errata iMX23 errata 5835.  The 5V disconnect problem is improved
 * by setting the 4p2 voltage level to equal the battery level.  When
 * testing a nominal part at room temperature (not sure if those
 * parameters affect the behavior), using a 12-ohm load on VDDIO, the VDDIO
 * voltage drop was less than 150mV.
 */
void PowerPrep_Handle5vDisconnectErrata( void )
{
	HW_POWER_DCDC4P2.B.BO=0;
	HW_POWER_DCDC4P2.B.TRG=4;
}


void PowerPrep_Enable4p2Fiq( void )
{
#ifndef mx28
	HW_ICOLL_CTRL_CLR( BM_ICOLL_CTRL_SFTRST );
	while( HW_ICOLL_CTRL.B.SFTRST == 1 );
	HW_ICOLL_CTRL_CLR( BM_ICOLL_CTRL_CLKGATE );
	while( HW_ICOLL_CTRL.B.CLKGATE == 1 );

	hw_icoll_EnableVector(VECTOR_IRQ_DCDC4P2_BO , 0);
	hw_icoll_SetFiqMode(VECTOR_IRQ_DCDC4P2_BO , 1);
	hw_icoll_CtrlRegisterUpdate(ICOLL_CTRL_FIQ_FINAL_ENABLE, 1);
	hw_core_EnableFiqInterrupt(true);


	// For 378x, we must also enable the IRQ to enable the FIQ.
	hw_icoll_EnableVector(VECTOR_IRQ_DCDC4P2_BO , 1);
#endif
}


void PowerPrep_InitDcdc4p2Source( void )
{

	if(HW_POWER_DCDC4P2.B.ENABLE_DCDC == false)
	{
		printf("Error: tried to enable 5VCTRL ENABLE_DCDC before\
			enabling DCDC4P2 ENABLE_DCDC.\n");
		return;
	}

	/* Enable Master 4p2 */
	hw_power_EnableDcdc( true );

	if(HW_POWER_CTRL_RD() & BM_POWER_CTRL_VBUSVALID_IRQ)
	{
		/* if we arrived here,  4P2 has been disabled  due to a VBUSVALID
		 * false condition (caused by actuall VDD5V voltage dipping or
		 * caused by glitch such as imx23 chip errata 5837) and sufficient
		 * battery power is not present.  Exiting to not enable DCDC
		 * power during 5V connected state.
		 */
		hw_power_EnableDcdc( false );
		hw_power_Enable4p2DcdcInput( false );
		hw_power_EnableMaster4p2( false );

		printf("Enabling of DCDC failed at setting of "\
			"5VCTRL ENABLE_DCDC.  The only 5V power supply "\
			"operating is the linear regulators.\r\n");
		return;
	}
}

int PowerPrep_5vBoot( void )
{
    if( IsVbusValid() && IsVdd5vGtVddio() )
    {
        // Both 5V detections see 5V.  Continue to boot device
        // with 5V.
        return PowerPrep_BootValid5v();
    }
    else
    {
	/* Delay before next check to give VDD5V more time to charge
	 * up */
	hw_digctl_MicrosecondWait(1000);
	if( IsVbusValid() && IsVdd5vGtVddio() )
	{
		return PowerPrep_BootValid5v();
	}
	else
	{
		// VBUS is not at a valid value, but VDD5V_GT_VDDIO is true.
		// This is a bad situation for the chip and we need to
		// handle it.
		return PowerPrep_Handle5vConflict();
	}
    }
}

int PowerPrep_Handle5vConflict( void )
{
    bool bLoop;

    // Need to check the VDDIO brownout level, so initialize it to maximum offset.
    HW_POWER_VDDIOCTRL.B.BO_OFFSET = BO_OFFSET_MAX;

    //--------------------------------------------------------------------------
    // The VBUSVALID and VDD5V_GT_VDDIO have different status
    // if we are here.  This is a bad state and we can't
    // continue to boot unless something changes.  So, we'll
    // stay in this loop.
    //--------------------------------------------------------------------------
    bLoop = true;
    while(bLoop)
    {
        //---------------------------------------------------------------------
        // Did output supply brownout?
        //---------------------------------------------------------------------
        if( HW_POWER_STS.B.VDDIO_BO )
        {
            // If VDDIO has a brownout, then the VDD5V_GT_VDDIO becomes
            // unreliable.  We need to shut down and try again.
            PowerPrep_PowerDown();
            return ERROR_VDDIO_BROWNOUT;
        }



        //---------------------------------------------------------------------
        // Did 5V change state?
        //---------------------------------------------------------------------
        if( IsVbusValid() && IsVdd5vGtVddio() )
        {
            // Both 5V detections see 5V.  Break out of loop and continue
            // to boot device with 5V and consider it a valid 5V.
            PowerPrep_BootValid5v();
            return SUCCESS;
        }

        if( Is5vPresent() == false )
        {
            // Neither 5V detection sees 5V.  It has dropped below
            // detection levels so we will shut down.
            PowerPrep_PowerDown();
            return ERROR_5V_OSCILLATING;
        }



        //---------------------------------------------------------------------
        // Was PSWITCH pressed?
        //---------------------------------------------------------------------
        if( HW_POWER_STS.B.PSWITCH > 0 )
        {
        	return PowerPrep_BattBoot();

        }
    } // end while(bLoop)

    return ERROR_UNEXPECTED_LOOP_EXIT;
}

int PowerPrep_InitBattBoFiq( void )
{
#ifndef mx28
    HW_ICOLL_CTRL_CLR( BM_ICOLL_CTRL_SFTRST );
    while( HW_ICOLL_CTRL.B.SFTRST == 1 );
    HW_ICOLL_CTRL_CLR( BM_ICOLL_CTRL_CLKGATE );
    while( HW_ICOLL_CTRL.B.CLKGATE == 1 );

    hw_icoll_EnableVector(VECTOR_IRQ_BATT_BRNOUT , 0);
    hw_icoll_SetFiqMode(VECTOR_FIQ_BATT_BRNOUT , 1);
    hw_icoll_CtrlRegisterUpdate(ICOLL_CTRL_FIQ_FINAL_ENABLE, 1);
    hw_core_EnableFiqInterrupt(true);


    // For 378x, we must also enable the IRQ to enable the FIQ.
    hw_icoll_EnableVector(VECTOR_IRQ_BATT_BRNOUT , 1);

#endif
    return SUCCESS;
}

int PowerPrep_InitBattBo( void )
{
	// Set up soft battery brownout
	HW_POWER_BATTMONITOR.B.BRWNOUT_LVL = BATTERY_BRWNOUT_BITFIELD_VALUE;

	BF_CLR( POWER_CTRL, BATT_BO_IRQ );
	BF_SET( POWER_CTRL, ENIRQBATT_BO );


	return SUCCESS;

}

int PowerPrep_Init4p2Bo( void )
{
	// Set up 4p2 rail brownout


	// Set the 4.2V rail brownout to 3.6V.
	HW_POWER_DCDC4P2.B.BO = 0;

	// Enable the brownout.
	BF_CLR( POWER_CTRL, DCDC4P2_BO_IRQ );
	BF_SET( POWER_CTRL, ENIRQ_DCDC4P2_BO );



	return SUCCESS;

}

int PowerPrep_Init4p2BoFiq( void )
{

#ifndef mx28
    // Enable the ICOLL block.
    HW_ICOLL_CTRL_CLR( BM_ICOLL_CTRL_SFTRST );
    while( HW_ICOLL_CTRL.B.SFTRST == 1 );
    HW_ICOLL_CTRL_CLR( BM_ICOLL_CTRL_CLKGATE );
    while( HW_ICOLL_CTRL.B.CLKGATE == 1 );

    // Setup the FIQ for 4.2V rail.
    hw_icoll_EnableVector(VECTOR_IRQ_DCDC4P2_BO , 0);
    hw_icoll_SetFiqMode(VECTOR_FIQ_DCDC4P2_BO , 1);
    hw_icoll_CtrlRegisterUpdate(ICOLL_CTRL_FIQ_FINAL_ENABLE, 1);
    hw_core_EnableFiqInterrupt(true);

    // For 378x, we must also enable the IRQ to enable the FIQ.
    hw_icoll_EnableVector(VECTOR_IRQ_DCDC4P2_BO , 1);
#endif

    return SUCCESS;
}

int PowerPrep_BattBoot( void )
{
#if 0
	if(!bBatteryReady)
	{
		/* wait until PSWITCH is no longer asserted to avoid
		* multiple boots under this condition */
		while((HW_POWER_STS.B.PSWITCH > 0 ) &&
		!bBatteryReady)
		{
			bBatteryReady = PowerPrep_IsBatteryReady();
		}
		PowerPrep_PowerDown();
	}
#endif

	/* enable battery brownout fiq to shutdown device if the battery
	* voltage gets to low, even if 5V is present.
	*/
	PowerPrep_InitBattBoFiq();
	HW_POWER_5VCTRL_CLR(BM_POWER_5VCTRL_PWDN_5VBRNOUT);
	/* now that protection is enabled, we can startup the DCDC converter */
	/*HW_POWER_5VCTRL_SET(BM_POWER_5VCTRL_ENABLE_DCDC);*/
	/* For maximum stability on the Vddd rail, we will leave the
	* Vddd linreg forced on (even under battery power).  The linreg
	* will only regulate if the target voltage get 25mV below the
	* DCDC target
	*/
	/*Disable DCDC*/
	HW_POWER_5VCTRL_CLR(BM_POWER_5VCTRL_ENABLE_DCDC);

	BF_CLR(POWER_DCDC4P2, ENABLE_DCDC);
	BF_CLR(POWER_DCDC4P2, ENABLE_4P2);
	BF_CLR(POWER_CHARGE,ENABLE_LOAD);

	/* Set DCDC_XFER to init DCDC for 100us*/
	BF_SET(POWER_5VCTRL, DCDC_XFER);
	hw_digctl_MicrosecondWait(1000);
	BF_CLR(POWER_5VCTRL, DCDC_XFER);

	BF_CLR(POWER_CTRL,ENIRQ_DCDC4P2_BO);

	/*Enable double FETs to make DCDC more powerful*/
	HW_POWER_MINPWR_CLR(BM_POWER_MINPWR_HALF_FETS);
	HW_POWER_MINPWR_SET(BM_POWER_MINPWR_DOUBLE_FETS);

	/* This should have already been done, but just in case */
	HW_POWER_VDDDCTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;
	HW_POWER_VDDACTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;
	HW_POWER_VDDIOCTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;

	/* Enabe DCDC output to Vddd rail */
	HW_POWER_VDDDCTRL.B.DISABLE_FET = 0;
	HW_POWER_VDDIOCTRL.B.DISABLE_FET = 0;
	HW_POWER_VDDACTRL.B.DISABLE_FET = 0;

	HW_POWER_VDDDCTRL.B.ENABLE_LINREG = 0;
	HW_POWER_VDDACTRL.B.ENABLE_LINREG = 0;

	HW_POWER_5VCTRL_SET(BM_POWER_5VCTRL_PWD_CHARGE_4P2);
	/* Enable DCDC*/
	HW_POWER_5VCTRL_SET(BM_POWER_5VCTRL_ENABLE_DCDC);

	BF_WR(POWER_5VCTRL,CHARGE_4P2_ILIMIT,0x8);
	/* Set current limit of Linear regulator to 0mA.*/
//	HW_POWER_5VCTRL_SET(BM_POWER_5VCTRL_ILIMIT_EQ_ZERO);

	return SUCCESS;
}

void PowerPrep_SwitchVdddToDdcdcSource( void )
{
	/* This should have already been done, but just in case */
	HW_POWER_VDDDCTRL.B.LINREG_OFFSET = HW_POWER_LINREG_OFFSET_STEP_BELOW;

	/* Enabe DCDC output to Vddd rail */
	HW_POWER_VDDDCTRL.B.DISABLE_FET = 0;
	/* For maximum stability on the Vddd rail, we will leave the
	 * Vddd linreg forced on (even under battery power).  The linreg
	 * will only regulate if the target voltage get 25mV below the
	 * DCDC target
	 */
	HW_POWER_VDDDCTRL.B.ENABLE_LINREG = 0;
	HW_POWER_VDDDCTRL.B.DISABLE_STEPPING = 0;

}

void PowerPrep_EnableOutputRailProtection( void )
{


	HW_POWER_CTRL_CLR(BM_POWER_CTRL_VDDD_BO_IRQ |
			BM_POWER_CTRL_VDDA_BO_IRQ |
			BM_POWER_CTRL_VDDIO_BO_IRQ);

	HW_POWER_VDDDCTRL.B.PWDN_BRNOUT = 1;
	HW_POWER_VDDACTRL.B.PWDN_BRNOUT = 1;

	/* due to imx23 errata 5835, we there is a possibility
	 * of large VDDIO droops in on a 5V disconnect.  If one
	 * occurs, we need to shutdown to protect the system
	 * against unknown conditions
	 */

	/* note that VDDIO brownout indicator has been found to falsely
	 * trigger due to a 5V connection.  Possibly also due to a pswitch
	 * press.
	 */
//#define     DISABLE_VDDIO_BO_PROTECTION //muryo
#ifndef	DISABLE_VDDIO_BO_PROTECTION
	HW_POWER_VDDIOCTRL.B.PWDN_BRNOUT = 1;
#endif

}

bool PowerPrep_IsBatteryReady( void )
{
	unsigned int BattVal = hw_power_GetBatteryVoltage();
	printf("Battery Voltage = ");
	PowerPrep_PrintBatteryVoltage(BattVal);
	if(hw_power_GetBatteryVoltage() >=
		MINIMUM_SAFE_BOOTING_BATTERY_VOLTAGE_MV)
		return true;
	else
		return false;
}

void putc(char ch)
{
	int loop = 0;
	while (HW_UARTDBGFR_RD()&BM_UARTDBGFR_TXFF) {
		loop++;
		if (loop > 10000)
			break;
	};

	/* if(!(HW_UARTDBGFR_RD() &BM_UARTDBGFR_TXFF)) */
	HW_UARTDBGDR_WR(ch);
}

void PowerPrep_PrintBatteryVoltage(unsigned int value)
{
	unsigned int num[3] = {0,0,0};
	unsigned int i = 0;

	num[0] = value / 1000;
	num[1] = value % 1000 / 100;
	num[2] = value % 100 / 10;

	for(i = 0;i < 3;i++)
	{
		if(i == 1)
			putc('.');
		if((num[i] >= 0) && (num[i] <= 9))
			putc('0' + num[i]);
	}
	putc('V');
}

void PowerPrep_CPUClock2XTAL(void)
{
#ifdef mx28
	HW_CLKCTRL_CLKSEQ_SET(BM_CLKCTRL_CLKSEQ_BYPASS_CPU);
#endif
}

void PowerPrep_CPUClock2PLL(void)
{
#ifdef mx28
	if((HW_CLKCTRL_PLL0CTRL0_RD() & BM_CLKCTRL_PLL0CTRL0_POWER)
		!= BM_CLKCTRL_PLL0CTRL0_POWER)
		HW_CLKCTRL_PLL0CTRL0_SET(BM_CLKCTRL_PLL0CTRL0_POWER);
	PowerPrep_Delay(100);
	/*HW_CLKCTRL_CPU_WR((HW_CLKCTRL_CPU_RD() & (~BM_CLKCTRL_CPU_DIV_CPU)) | 0x14);*/
	if((HW_CLKCTRL_CLKSEQ_RD() & BM_CLKCTRL_CLKSEQ_BYPASS_CPU)
		== BM_CLKCTRL_CLKSEQ_BYPASS_CPU)
		HW_CLKCTRL_CLKSEQ_CLR(BM_CLKCTRL_CLKSEQ_BYPASS_CPU);
#endif
}

bool PowerPrep_IsBatteryGood()
{
	unsigned int u32BatteryVoltage = 0;
	u32BatteryVoltage = HW_POWER_BATTMONITOR.B.BATT_VAL * 8;
	if((u32BatteryVoltage > BATTERY_LOW) && (u32BatteryVoltage < BATTERY_HIGH))
		return TRUE;
	else
	{
		BF_WR(POWER_5VCTRL, CHARGE_4P2_ILIMIT, 0x3);
		HW_POWER_5VCTRL_CLR(BM_POWER_5VCTRL_PWD_CHARGE_4P2);
		PowerPrep_SetCharger(0x3);
		hw_digctl_MicrosecondWait(500000);
		u32BatteryVoltage = HW_POWER_BATTMONITOR.B.BATT_VAL * 8;
		if(u32BatteryVoltage > BATTERY_BAD)
			return FALSE;
		else if(u32BatteryVoltage > BATTERY_LOW)
			return TRUE;
	}
	PowerPrep_StopCharger();
	return FALSE;
}

void PowerPrep_ChargeBattery2Boot()
{
	unsigned int i = 0,j = 0;
	unsigned BattVoltage = 0;
	printf("Battery is too low,need charging......(3.4V)\r\n");

	/*Check the battery  voltage twice.*/
	while(i < 2)
	{
		BattVoltage = 0;
		while(BattVoltage < BATTERY_BOOT)
		{
			PowerPrep_StopCharger();
			PowerPrep_Delay(500000);
			BattVoltage = HW_POWER_BATTMONITOR.B.BATT_VAL * BATT_VOLTAGE_8_MV;
			printf("\rBATT:");
			PowerPrep_PrintBatteryVoltage(BattVoltage);
			printf("   ");
			PowerPrep_SetCharger(0x4);
			/*50mA if it is VBUS 5V.
			*Print rotary cursor,wait for 20 seconds
			*/
			j = 0;
			while(j++ < 25)
			{
				printf("-\b");
				PowerPrep_Delay(200000);
				printf("\\\b");
				PowerPrep_Delay(200000);
				printf("|\b");
				PowerPrep_Delay(200000);
				printf("/\b");
				PowerPrep_Delay(200000);
			}
		}
		i++;
	}
	PowerPrep_StopCharger();

}

void PowerPrep_SetCharger(unsigned int current)
{
	BF_CLRV(POWER_CHARGE, STOP_ILIMIT, 0xF);
	BF_SETV(POWER_CHARGE, STOP_ILIMIT, 0x1);
	BF_CLRV(POWER_CHARGE, BATTCHRG_I, 0x3F);
	BF_SETV(POWER_CHARGE, BATTCHRG_I, current);
	BF_CLR(POWER_CHARGE, PWD_BATTCHRG);
	BF_CLR(POWER_5VCTRL, PWD_CHARGE_4P2);
}

void PowerPrep_StopCharger()
{
	BF_CLRV(POWER_CHARGE, STOP_ILIMIT, 0xF);
	BF_CLRV(POWER_CHARGE, BATTCHRG_I, 0x3F);
	BF_SET(POWER_CHARGE, PWD_BATTCHRG);
}
void PowerPrep_Delay(unsigned int microSec)
{
	unsigned int currentTime = HW_DIGCTL_MICROSECONDS_RD();
	while ((HW_DIGCTL_MICROSECONDS_RD() - currentTime) <  microSec);
}

// eof power_prep.c
//! @}
// eof power_prep.c
//! @}
