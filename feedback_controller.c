/*******************************************************************************
* feedback_controller.c
*
* Here lies the heart and soul of the operation. I wish the whole flight
* controller could be just this file, woe is me. initialize_controllers()
* pulls in the control constants from json_settings and sets up the 
* discrete controllers. From then on out, feedback_controller() should be called
* by the IMU interrupt at feedbacl_hz until the program is shut down. 
* feedback_controller() will monitor the setpoint which is constantly being 
* changed by setpoint_manager(). It also does state estimation to update 
* core_state() even when the controller is disarmed. When controllers are
* enabled or disabled mid-flight by mode switches then the controllers are
* started smoothly.
*******************************************************************************/

#include <stdio.h>
#include <roboticscape.h>
#include "fly_defs.h"
#include "fly_types.h"
#include "fly_function_declarations.h"

// local arm state, set this from outside with arm/disarm_controller
arm_state_t arm_state;

// discrete controllers
fly_controllers_t controllers;
d_filter_t D_roll, D_pitch, D_yaw;

// keep original controller gains for scaling later
double D_roll_gain_orig, D_pitch_gain_orig, D_yaw_gain_orig;

// pointers to outside structs
setpoint_t* sp;
cstate_t* cs;
imu_data_t* imu;
fly_settings_t* set;

// one log entry, passed to log manager if logging enabled
log_entry_t new_log;

int num_yaw_spins;
int last_yaw;
double u[6], mot[8], tmp;
uint64_t loop_index;

// altitude controller need setup if being turned on mid flight
// so keep track of last state to detect changes.
int last_en_alt_ctrl;
double last_usr_thr;


// Local functions only for use in this c file
int set_motors_to_idle();
int zero_out_controller();
int feedback_controller();

/*******************************************************************************
* int disarm_controller()
*	
* This is how outside functions should stop the flight controller.
* it would be reasonable to set motors to 0 here, but since this function can
* be called from anywhere that might produce conflicts. Instead the interrupt
* service routine will do this on the next loop after disarming to maintain
* timing of pulses to the motors
*******************************************************************************/
int disarm_controller(){
	arm_state = DISARMED;
	set_led(RED,1);
	set_led(GREEN,0); 
	stop_log_manager();
	return 0;
}

/*******************************************************************************
* int disarm_controller()
*	
* This is how outside functions should stop the flight controller.
*******************************************************************************/
int arm_controller(){
	if(arm_state==ARMED){
		printf("WARNING: trying to arm when controller is already armed\n");
		return -1;
	}
	// start a new log file every time controller is armed, this may take some
	// time so do it before touching anything else
	if(set.enable_logging) start_log_manager();
	// zero the controllers so they start fresh
	zero_out_controller();
	set_led(RED,0);
	set_led(GREEN,1); 

	// last thing is to flag as armed 
	arm_state = ARMED;
	return 0;
}

/*******************************************************************************
* arm_state_t get_controller_arm_state()
*	
* Returns the arm state of the controller so outside functions, namely the
* setpoint_manager, can tell if the controller is armed or not.
*******************************************************************************/
arm_state_t get_controller_arm_state(){
	return arm_state;
}

/*******************************************************************************
* initialize_controller()
*
* initial setup of all feedback controllers. Should only be called once on
* program start. 
*******************************************************************************/
int initialize_controller(cstate_t* cstate, setpoint_t* setpoint, \
									imu_data_t* imu_data, fly_settings_t* settings){

	// make local copies of pointers to global structs
	cs = cstate;
	sp = setpoint;
	imu = imu_data;
	set = settings;

	// get controllers from settings
	if(get_json_roll_controller(&D_roll)) return -1;
	if(get_json_pitch_controller(&D_pitch)) return -1;
	if(get_json_yaw_controller(&D_yaw)) return -1;

	// save original gains as we will scale these by battery voltage later
	D_roll_gain_orig = D_roll.gain;
	D_pitch_gain_orig = D_pitch.gain;
	D_yaw_gain_orig = d_yaw.gain;

	// enable soft start
	enable_soft_start(&D_roll, SOFT_START_SECONDS);
	enable_soft_start(&D_pitch, SOFT_START_SECONDS);
	enable_soft_start(&D_yaw, SOFT_START_SECONDS);

	// make sure everything is disarmed them start the ISR
	disarm_controller();
	set_imu_interrupt_func(&feedback_controller);
	return 0;
}

/*******************************************************************************
* zero_out_controller()
*
* clear the controller memory
*******************************************************************************/
int zero_out_controller(){
	reset_filter(&D_roll);
	reset_filter(&D_pitch);
	reset_filter(&D_yaw);

	// when swapping from direct throttle to altitude control, the altitude
	// controller needs to know the last throttle input for smooth transition
	last_alt_ctrl_en = 0;
	last_usr_thr = MIN_THRUST_COMPONENT;

	// yaw estimator can be zero'd too
	num_yaw_spins = 0;
	last_yaw = -imu->fusedTaitBryan[VEC3_Z]; // minus because NED coordinates
	return 0;
}

/*******************************************************************************
* int set_motors_to_idle()
*
* send slightly negative throttle to ESCs which keeps them awake but doesn't
* move the motors.
*******************************************************************************/
int set_motors_to_idle(){
	int i;
	if(set->num_rotors>8){
		printf("ERROR: set_motors_to_idle: too many rotors\n");
		return -1;
	}
	for(i=1;i<=set->num_rotors;i++) send_esc_pulse_normalized(i,-0.1);
	return 0;
}


/*******************************************************************************
* feedback_controller()
*	
* Should be called by the IMU interrupt at SAMPLE_RATE_HZ
*******************************************************************************/
int feedback_controller(){
	int i;
	double tmp, min, max;
	double mot[ROTORS];
	
	/***************************************************************************
	*	STATE_ESTIMATION
	*	read sensors and compute the state regardless of if the controller
	*	is ARMED or DISARMED
	***************************************************************************/
	// collect new IMU roll/pitch data
	// to remain consistent with NED coordinates we flip X&Y
	cs->roll   = imu->fusedTaitBryan[VEC3_Y];
	cs->pitch  = imu->fusedTaitBryan[VEC3_X];

	// yaw is more annoying since we have to detect spins
	// also make sign negative since NED coordinates has Z point down
	tmp = -imu->fusedTaitBryan[VEC3_Z] + (num_yaw_spins * TWO_PI);
	// detect the crossover point at +-PI and write new value to core state
	if(tmp-last_yaw < -PI) num_yaw_spins++;
	else if (tmp-last_yaw > PI) num_yaw_spins--;
	// finally num_yaw_spins is updated and the new value can be written 
	cs->yaw = imu->fusedTaitBryan[VEC3_Z] + (num_yaw_spins * TWO_PI);
	last_yaw = cs->yaw;

	// TODO: altitude estimate

	/***************************************************************************
	* Now check for all conditions that prevent normal running
	***************************************************************************/
	// Disarm if rc_state is somehow paused without disarming the controller.
	// This shouldn't happen if other threads are working properly.
	if(get_state()!=RUNNING && arm_state==ARMED){
		disarm_controller();
		last_arm_state = DISARMED;
	}

	// check for a tipover
	if(fabs(cs->roll)>TIP_ANGLE || fabs(cs->pitch)>TIP_ANGLE){
		disarm_controller();
		printf("\n TIPOVER DETECTED \n");
		set_motors_to_idle();
		last_arm_state = DISARMED;
		return 0;
	}

	/***************************************************************************
	* if not running or not armed, keep the motors in an idle state
	***************************************************************************/
	if(get_state()!=RUNNING || arm_state==DISARMED){
		set_motors_to_idle();
		return 0;
	}

	/***************************************************************************
	* We are about to start marching the individual SISI controllers forward.
	* Start by zeroing out the motors signals then add from there.
	***************************************************************************/
	for(i=0;i<set->num_rotors;i++) mot[i] = 0.0;


	/***************************************************************************
	* Throttle/Altitude Controller
	*
	* If transitioning from direct throttle to altitude control, prefill the 
	* filter with current throttle input to make smooth transition. This is also
	* true if taking off for the first time in altitude mode as arm_controller 
	* sets up last_alt_ctrl_en and last_usr_thr every time controller arms
	***************************************************************************/
	// // run altitude controller if enabled
	// if(sp->en_alt_ctrl){
	// 	if(last_alt_ctrl_en == 0){
	// 		sp->altitude = cs->alt; // set altitude setpoint to current altitude
	// 		reset_filter(&D0);
	// 		prefill_filter_outputs(&D0,last_usr_thr);
	// 		last_alt_ctrl_en = 1;
	// 	}
	// 	sp->altitude += sp->altitude_rate*DT;
	// 	saturate_double(&sp->altitude, cs->alt-ALT_BOUND_D, cs->alt+ALT_BOUND_U);
	// 	D0.gain = D0_GAIN * V_NOMINAL/cs->vbatt;
	// 	tmp = march_filter(&D0, sp->altitude-cs->alt);
	// 	u[VEC_THR] = tmp / cos(cs->roll)*cos(cs->pitch);
	// 	saturate_double(&u[VEC_THR], MIN_THRUST_COMPONENT, MAX_THRUST_COMPONENT);
	// 	add_mixed_input(u[VEC_THR], VEC_THR, mot);
	// 	last_alt_ctrl_en = 1;
	// }
	// // else use direct throttle
	// else{

	// compensate for tilt
	tmp = sp->Z_throttle / (cos(cs->roll)*cos(cs->pitch));
	saturate_double(&tmp, -MIN_THRUST_COMPONENT, -MAX_THRUST_COMPONENT);
	u[VEC_THR] = tmp;
	add_mixed_input(u[VEC_THR], VEC_THR, mot);
	// save throttle in case of transition to altitude control
	last_usr_thr = sp->Z_throttle; 
	last_alt_ctrl_en = 0;

	/***************************************************************************
	* Roll Pitch Yaw controllers, only run if enabled         
	***************************************************************************/
	if(sp->en_rpy_ctrl){
		// Roll
		check_channel_saturation(VEC_ROLL, mot, &min, &max);
		if(max>MAX_ROLL_COMPONENT)  max =  MAX_ROLL_COMPONENT;
		if(min<-MAX_ROLL_COMPONENT) min = -MAX_ROLL_COMPONENT;
		enable_saturation(&D_roll, min, max);
		D_roll.gain = D_roll_gain_orig * set->v_nominal/cs->v_batt;
		u[VEC_ROLL]=march_filter(&D_roll, sp->roll - cs->roll);
		add_mixed_input(u[VEC_ROLL], VEC_ROLL, mot);

		// pitch
		check_channel_saturation(VEC_PITCH, mot, &min, &max);
		if(max>MAX_PITCH_COMPONENT)  max =  MAX_PITCH_COMPONENT;
		if(min<-MAX_PITCH_COMPONENT) min = -MAX_PITCH_COMPONENT;
		enable_saturation(&D_pitch, min, max);
		D_pitch.gain = D_pitch_gain_orig * set->v_nominal/cs->v_batt;
		u[VEC_PITCH] = march_filter(&D_pitch, sp->pitch - cs->pitch);
		add_mixed_input(u[VEC_PITCH], VEC_PITCH, mot);

		// Yaw
		// if throttle stick is down (waiting to take off) keep yaw setpoint at
		// current heading, otherwide update by yaw rate
		sp->yaw += DT*sp->yaw_rate;
		check_channel_saturation(VEC_YAW, mot, &min, &max);
		if(max>MAX_YAW_COMPONENT)  max =  MAX_YAW_COMPONENT;
		if(min<-MAX_YAW_COMPONENT) min = -MAX_YAW_COMPONENT;
		enable_saturation(&D_yaw, min, max);
		D_yaw.gain = D_yaw_gain_orig * set->v_nominal/cs->v_batt;
		u[VEC_YAW] = march_filter(&D_yaw, sp->yaw - cs->yaw);
		add_mixed_input(u[VEC_YAW], VEC_YAW, mot);
	}
	else{
		u[VEC_ROLL]		= 0.0;
		u[VEC_PITCH]	= 0.0;
		u[VEC_YAW]		= 0.0;
	}

	/***************************************************************************
	* X (Side) and Y (Forward) inputs, only when 6dof is enabled
	***************************************************************************/
	if(sp->en_6dof){
		// Y (sideways)
		u[VEC_Y] = sp->X_throttle;
		check_channel_saturation(VEC_Y, mot, &min, &max);
		if(max>MAX_X_COMPONENT)  max =  MAX_X_COMPONENT;
		if(min<-MAX_X_COMPONENT) min = -MAX_X_COMPONENT;
		saturate_double(&u[VEC_Y], min, max);
		// add mixed components to the motors
		add_mixed_input(u[VEC_Y], VEC_Y, mot);

		// X (forward)
		u[VEC_X] = sp->Y_throttle;
		check_channel_saturation(VEC_X, mot, &min, &max);
		if(max>MAX_Y_COMPONENT)  max =  MAX_Y_COMPONENT;
		if(min<-MAX_Y_COMPONENT) min = -MAX_Y_COMPONENT;
		saturate_double(&u[VEC_X], min, max);
		// add mixed components to the motors
		add_mixed_input(u[VEC_X], VEC_Y, mot);
	}
	else{
		u[VEC_Y] = 0.0;
		u[VEC_X] = 0.0;
	}

	/***************************************************************************
	* Send ESC motor signals immediately at the end of the control loop
	***************************************************************************/
	for(i=0;i<set.num_rotors;i++){
		// write motor signals to cstate before final saturation so errors
		// may show up in the logs
		cs->m[i] = new_mot[i];
		// Final saturation before sending motor signals to prevent errors
		saturate_double(&new_mot[i], 0.0, 1.0);
		send_esc_pulse_normalized(i+1,new_mot[i]);
	}

	/***************************************************************************
	* Add new log entry
	***************************************************************************/
	if(set.enable_logging){
		new_log.loop_index	= loop_index;
		new_log.alt			= cs->alt;
		new_log.roll		= cs->roll;
		new_log.pitch		= cs->pitch;
		new_log.yaw			= cs->yaw;
		new_log.vbatt		= cs->vbatt;
		new_log.u_thr		= u[VEC_THR];
		new_log.u_roll		= u[VEC_ROLL];
		new_log.u_pitch		= u[VEC_PITCH];
		new_log.u_yaw		= u[VEC_YAW];
		new_log.u_X			= u[VEC_Y];
		new_log.u_Y			= u[VEC_X];
		new_log.mot_1		= cs->m[0];
		new_log.mot_2		= cs->m[1];
		new_log.mot_3		= cs->m[2];
		new_log.mot_4		= cs->m[3];
		new_log.mot_5		= cs->m[4];
		new_log.mot_6		= cs->m[5];
		add_log_entry(new_log);
	}

	loop_index++;
	return 0;
}