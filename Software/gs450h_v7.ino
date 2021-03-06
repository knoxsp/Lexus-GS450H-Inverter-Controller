/*Basic software to run the Lexus GS450H hybrid transmission and inverter using the open source V1 controller
 * Take an analog throttle signal and converts to a torque command to MG1 and MG2
 * Feedback provided over USB serial
 * V3 Reverse added
 * V4 CAN specific to BMW E65 735i project
 * 
 *  V5  WiFi connection on USART2 at 19200 baud
 * 
 * V6 - add ISA CAN shunt connectivity. Note V2 hardware only. 
 * 
 * 
 * V7 - add HV precharge and control- oil pump relay=midpack and precharge contactor, out1= main contactor.
 * 
 * Copyright 2019 T.Darby , D.Maguire
 * openinverter.org
 * evbmw.com
 * 
 */


#include <Metro.h>
#include <due_can.h>  //https://github.com/collin80/due_can
#include <due_wire.h> //https://github.com/collin80/due_wire
#include <DueTimer.h>  //https://github.com/collin80/DueTimer
#include <Wire_EEPROM.h> //https://github.com/collin80/Wire_EEPROM
#include <ISA.h>  //isa can shunt library


#define MG2MAXSPEED 10000
#define pin_inv_req 22

#define PARK 0
#define REVERSE 1
#define NEUTRAL 2
#define DRIVE 3

#define OilPumpPower  33
#define OilPumpPWM  2
#define InvPower    34
#define Out1  50
#define TransSL1  47
#define TransSL2  44
#define TransSP   45

#define IN1   6
#define IN2   7
#define Brake_In   62

#define TransPB1    40
#define TransPB2    43
#define TransPB3    42

#define HVPreset    20 //voltage at which to enable main contactor

#define OilpumpTemp A7
#define TransTemp A4
#define MG1Temp A5
#define MG2Temp A6

////////////////Global variables ////////////////////////////////////////////////////////////
word RPM;
byte  Gcount; //gear display counter byte
unsigned int GLeaver;  //unsigned int to contain result of message 0x192. Gear selector lever position
int shiftPos; //contains byte to display gear position on dash
byte gear;
byte mthCnt;
////////////////////////////////////////////////////////////////////////////////////////////////////
CAN_FRAME outframe;  //A structured variable according to due_can library for transmitting CAN data.

byte get_gear()
{
  if(digitalRead(IN1))
  {
    return(DRIVE);
  }else if (digitalRead(IN2)){
    return(REVERSE);
  }else{
    return(NEUTRAL);
  }
}

Metro timer_htm=Metro(10); 
Metro timer_diag = Metro(1100);
Metro timer_Frames200 = Metro(200);
Metro timer_Frames10 = Metro(10);

byte mth_data[100];
byte htm_data_setup[80]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,25,0,0,0,0,0,0,0,128,0,0,0,128,0,0,0,37,1};
byte htm_data[80]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0,0,0,0,0,0,0,0,0}; 

unsigned short htm_checksum=0, 
               mth_checksum=0, 
               since_last_packet=4000;

unsigned long  last_packet=0;
            
volatile byte mth_byte=0;

float isa_dc_bus_voltage=0, dc_bus_voltage=0,temp_inv_water=0,temp_inv_inductor=0; //just used for diagnostic output

short mg1_torque=0,
      mg2_torque=0,
      mg1_speed=-1,
      mg2_speed=-1;
       
byte inv_status=1;
//     gear=get_gear(); //get this from your transmission
     
bool htm_sent=0, 
     mth_good=0;

//int oil_power=135; //oil pump pwm value
int oil_power=100; //oil pump pwm value

//Throttle Pedal Configuration
int ThrotlLow = 160; // 75 is the default
int ThrotlHi = 520;  // 370 is the default
int ThrotlMargin = 5; //dead zone at start of throttle travel

//default is 3500
static int16_t MaxTrq=3500;  //max torque request
static int16_t MinTrq = MaxTrq*-1;

int Throt1Pin = A0; //throttle pedal analog inputs
int Throt2Pin = A1;
int ThrotVal=0; //value read from throttle pedal analog input
bool T15Status; //flag to keep status of Terminal 15 from CAS via CAN.
bool dash_status; //flag for dash on can command.
bool can_status;  //flag for turning off and on can sending.
     


short get_torque()
{
  //accelerator pedal mapping to torque values here
  ThrotVal=analogRead(Throt1Pin);
  if (ThrotVal<(ThrotlLow+5)) ThrotVal=ThrotlLow;//dead zone at start of throttle travel
  if(gear==DRIVE) {ThrotVal = map(ThrotVal, ThrotlLow, ThrotlHi, 0, MaxTrq);}
  else if(gear==REVERSE) {ThrotVal = map(ThrotVal, ThrotlLow, ThrotlHi, 0,MinTrq);}
  else if(gear==PARK) {ThrotVal = 0;}  //no torque in park or neutral
  else if(gear==NEUTRAL) {ThrotVal = 0;}  //no torque in park or neutral
  return ThrotVal; //return torque
}


ISA Sensor;  //Instantiate ISA Module Sensor object to measure current and voltage 

#define SerialDEBUG SerialUSB

void setup() {

  SerialDEBUG.print("Setting Up");

   Can0.begin(CAN_BPS_500K);  //CAN bus for V2. Use for isa shunt comms etc
   Can1.begin(CAN_BPS_500K);  //CAN bus for communication with E65 PT CAN
   Can1.watchForRange(0x130, 0x192);  // only receive messages from 0x130 to 0x192
  // Can1.attachCANInterrupt(Incoming); // SK Stop processing data from CAN 1 until i've got the rx8 dash set up.

    Sensor.begin(0,500);  //Start ISA object on CAN 0 at 500 kbps
   
//   Timer4.attachInterrupt(Frames10MS).start(10000); // Send frames every 10ms
//   Timer3.attachInterrupt(Frames200MS).start(200000); // Send frames every 200ms
 //set initial conditions/////////////////
    T15Status=false;
    dash_status=false;
    can_status=false;
    RPM=750;
    Gcount=0x0d;
    gear=NEUTRAL;
    shiftPos=0xb4; //select neutral
//////////////////////////////////////////////   
  pinMode(pin_inv_req, OUTPUT);
  digitalWrite(pin_inv_req, 1);
  pinMode(13, OUTPUT);  //led
  pinMode(OilPumpPower, OUTPUT);  //Oil pump control relay- being used to control HV precharge contactor as not required seperate oil pump power control.
  digitalWrite(OilPumpPower,LOW);  //turn off precharge
  //analogWrite(OilPumpPWM,125);  //set 50% pwm to oil pump at 1khz for testing

  pinMode(InvPower, OUTPUT);  //Inverter Relay 
  pinMode(Out1, OUTPUT);  //GP output one
  pinMode(TransSL1,OUTPUT); //Trans solenoids
  pinMode(TransSL2,OUTPUT); //Trans solenoids
  pinMode(TransSP,OUTPUT); //Trans solenoids

  digitalWrite(InvPower,LOW);  //turn off at startup
  digitalWrite(Out1,LOW);  //turn off HV main at startup
  digitalWrite(TransSL1,LOW);  //turn off at startup
  digitalWrite(TransSL2,LOW);  //turn off at startup
  digitalWrite(TransSP,LOW);  //turn off at startup

  pinMode(IN1,INPUT); //Input 1
  pinMode(IN2,INPUT); //Input 2
  pinMode(Brake_In,INPUT); //Brake pedal input

  pinMode(TransPB1,INPUT); //Trans inputs
  pinMode(TransPB2,INPUT); //Trans inputs
  pinMode(TransPB3,INPUT); //Trans inputs

  Serial1.begin(250000);

  PIOA->PIO_ABSR |= 1<<17;
  PIOA->PIO_PDR |= 1<<17;
  USART0->US_MR |= 1<<4 | 1<<8 | 1<<18;

  htm_data[63]=(-5000)&0xFF;  // regen ability of battery
  htm_data[64]=((-5000)>>8);

  htm_data[65]=(27500)&0xFF;  // discharge ability of battery
  htm_data[66]=((27500)>>8);
 
  SerialDEBUG.begin(115200);

  Serial2.begin(19200); //setup serial 3 for wifi access
}



void HV_On()
{
if (dash_status==true || IN1==HIGH) //if we turn on the car or call for hv for charging then turn on hv
  {
digitalWrite(OilPumpPower,HIGH);  //turn on hv precharge and pack split contactors

  if (isa_dc_bus_voltage>HVPreset)
{
digitalWrite(Out1,HIGH);  //turn on hv main once precharge complete
}
  
  }
  
}

void HV_Off()
{
  digitalWrite(Out1,LOW);  //turn of hv main contactor
  digitalWrite(OilPumpPower,LOW);  //turn of hv precharge and pack split contactors 
}


void handle_wifi(){
/*
 * 
 * Routine to send data to wifi on serial 2
The information will be provided over serial to the esp8266 at 19200 baud 8n1 in the form :
vxxx,ixxx,pxxx,mxxxx,nxxxx,oxxx,rxxx,qxxx* where :

v=pack voltage (0-700Volts)
i=current (0-1000Amps)
p=power (0-300kw)
m=mg1 rpm (0-10000rpm)
n=mg2 rpm (0-10000rpm)
o=mg1 temp (-20 to 120C)
r=mg2 temp (-20 to 120C)
q=oil pressure (0-100%)
*=end of string
xxx=three digit integer for each parameter eg p100 = 100kw.
updates will be every 100ms approx.

v100,i200,p35,m3000,n4000,o20,r100,q50*
*/
  
//Serial2.print("v100,i200,p35,m3000,n4000,o20,r30,q50*"); //test string

//digitalWrite(13,!digitalRead(13));//blink led every time we fire this interrrupt.

Serial2.print("v");//dc bus voltage
Serial2.print(isa_dc_bus_voltage);
Serial2.print(",i");//dc current
Serial2.print(Sensor.Amperes);//current derived from ISA shunt
Serial2.print(",p");//total motor power
Serial2.print(Sensor.KW);//Power value derived from ISA Shunt
Serial2.print(",m");//mg1 rpm
Serial2.print(mg1_speed);
Serial2.print(",n");//mg2 rpm
Serial2.print(mg2_speed);
Serial2.print(",o");//mg1 temp. Using water temp for now
Serial2.print(temp_inv_water);
Serial2.print(",r");//mg2 temp. Using water temp for now
Serial2.print(temp_inv_water);
Serial2.print(",q");// pwm percent on oil pump
Serial2.print(map(oil_power, 0, 255, 0, 100));// change oil pump pwm value to percent.
Serial2.print("*");// end of data indicator

}






void control_inverter() {

  int speedSum=0;

  if(timer_htm.check()) //prepare htm data
  {
    if(mth_good)
    {
      dc_bus_voltage=(((mth_data[82]|mth_data[83]<<8)-5)/2);
      temp_inv_water=(mth_data[42]|mth_data[43]<<8);
      temp_inv_inductor=(mth_data[86]|mth_data[87]<<8);
      mg1_speed=mth_data[6]|mth_data[7]<<8;
      mg2_speed=mth_data[31]|mth_data[32]<<8;
    }
    gear=get_gear();
    mg2_torque=get_torque(); // -3500 (reverse) to 3500 (forward)
    mg1_torque=((mg2_torque*5)/4);
    //SerialDEBUG.print("MG1 Torque: "); SerialDEBUG.println(mg1_torque);
    //SerialDEBUG.print("MG2 Torque: "); SerialDEBUG.println(mg2_torque);
    if((mg2_speed>MG2MAXSPEED)||(mg2_speed<-MG2MAXSPEED))mg2_torque=0;
    if(gear==REVERSE)mg1_torque=0;

    //speed feedback
    speedSum=mg2_speed+mg1_speed;
    speedSum/=113;
    htm_data[0]=(byte)speedSum;
    htm_data[75]=(mg1_torque*4)&0xFF;
    htm_data[76]=((mg1_torque*4)>>8);
    
    //mg1
    htm_data[5]=(mg1_torque*-1)&0xFF;  //negative is forward
    htm_data[6]=((mg1_torque*-1)>>8);
    htm_data[11]=htm_data[5];
    htm_data[12]=htm_data[6];

    //mg2
    htm_data[26]=(mg2_torque)&0xFF; //positive is forward
    htm_data[27]=((mg2_torque)>>8);
    htm_data[32]=htm_data[26];
    htm_data[33]=htm_data[27];

    //checksum
    htm_checksum=0;
    for(byte i=0;i<78;i++)htm_checksum+=htm_data[i];
    htm_data[78]=htm_checksum&0xFF;
    htm_data[79]=htm_checksum>>8;
  }
  
  since_last_packet=micros()-last_packet;

  if(since_last_packet>=4000) //read mth
  {    
    htm_sent=0;
    mth_byte=0;
    mth_checksum=0;
    
    for(int i=0;i<100;i++)mth_data[i]=0;
    while(Serial1.available()){mth_data[mth_byte]=Serial1.read();mth_byte++;}
    
    for(int i=0;i<98;i++)mth_checksum+=mth_data[i];
    if(mth_checksum==(mth_data[98]|(mth_data[99]<<8)))mth_good=1;else mth_good=0;
    last_packet=micros();
    digitalWrite(pin_inv_req,0);
  }

  since_last_packet=micros()-last_packet;
  
  if(since_last_packet>=10)digitalWrite(pin_inv_req,1);

  if(since_last_packet>=1000)
  {
    if(!htm_sent&&inv_status==0){for(int i=0;i<80;i++)Serial1.write(htm_data[i]);}
    else if(!htm_sent&&inv_status!=0){for(int i=0;i<80;i++)Serial1.write(htm_data_setup[i]);if(mth_data[1]!=0) inv_status--;}
    htm_sent=1;
  }
}




void diag_mth()
{
  ///mask just hides any MTH data byte which is represented here with a 0. Useful for debug/discovering.
  bool mth_mask[100] = {
    0,0,0,0,0,0,0,0,1,1,
    1,1,0,0,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,
    1,0,0,1,1,1,1,0,0,1,
    1,1,0,0,1,1,1,1,1,1,
    1,1,1,1,1,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    1,1,0,0,1,1,0,0,1,1,
    1,1,1,1,1,1,1,1,0,0,};
    
  SerialDEBUG.print("\n");
  SerialDEBUG.println("\t0\t1\t2\t3\t4\t5\t6\t7\t8\t9");
  SerialDEBUG.println("   ------------------------------------------------------------------------------");  
  for (int j=0;j<10;j++)
  {
    SerialDEBUG.print(j*10);if(j==0)SerialDEBUG.print("0");SerialDEBUG.print(" |\t");
    for (int k=0;k<10;k++)
    {
      if(mth_mask[j*10+k])SerialDEBUG.print(mth_data[j*10+k]);else SerialDEBUG.print (" ");
      SerialDEBUG.print("\t");
    }
    SerialDEBUG.print("\n");
  }
  SerialDEBUG.print("\n");
    
  SerialDEBUG.print("MTH Valid: ");if(mth_good)SerialDEBUG.print("Yes"); else SerialDEBUG.print("No");SerialDEBUG.print("\tChecksum: ");SerialDEBUG.print(mth_checksum);
  SerialDEBUG.print("\n");
  
  SerialDEBUG.print("ISA DC Bus: ");if(isa_dc_bus_voltage>=0)SerialDEBUG.print(isa_dc_bus_voltage);else SerialDEBUG.print(isa_dc_bus_voltage);
  SerialDEBUG.print("v\n");

  SerialDEBUG.print("Inverter DC Bus: ");if(dc_bus_voltage>=0)SerialDEBUG.print(dc_bus_voltage);else SerialDEBUG.print(dc_bus_voltage);
  SerialDEBUG.print("v\n");
 
  SerialDEBUG.print("MG1 - Speed: ");SerialDEBUG.print(mg1_speed);
  SerialDEBUG.print("rpm\tPosition: ");SerialDEBUG.print(mth_data[12]|mth_data[13]<<8);
  SerialDEBUG.print("\n");
  
  SerialDEBUG.print("MG2 - Speed: ");SerialDEBUG.print(mg2_speed);
  SerialDEBUG.print("rpm\tPosition: ");SerialDEBUG.print(mth_data[37]|mth_data[38]<<8);
  SerialDEBUG.print("\n");
  
  SerialDEBUG.print("Water Temp:\t");SerialDEBUG.print(temp_inv_water);
  SerialDEBUG.print("c\nInductor Temp:\t" );SerialDEBUG.print(temp_inv_inductor);
  SerialDEBUG.print("c\nAnother Temp:\t");SerialDEBUG.print(mth_data[88]|mth_data[89]<<8);
  SerialDEBUG.print("c\nAnother Temp:\t");SerialDEBUG.print(mth_data[41]|mth_data[40]<<8);
  SerialDEBUG.print("c\n");
  
  SerialDEBUG.print("Gear: ");
  if (gear == DRIVE){
    SerialDEBUG.println("Drive");
  }else if (gear == REVERSE){
    SerialDEBUG.println("Reverse");
  }else {
    SerialDEBUG.println("Neutral");
  }
  
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
  SerialDEBUG.print("\n");
}



/////////////////////////////////////////////////////////////////////////////////////////////////////
///////Handle incomming pt can messages from the car here
////////////////////////////////////////////////////////////////////////////////////////////////////
void Incoming (CAN_FRAME *frame){

    ///////////Message from CAS on 0x130 byte one for Terminal 15 wakeup
  
      if(frame->id==0x130)
      {
        if(frame->data.byte[0] == 0x45) T15Status=true; //if the cas sends 0x45 in byte 0 of id 0x130 we have a run command
        else T15Status=false;
      }
      
      
 ////////////////////////////////////////////////////////////////////////////////////////////////////////////////     

  //////////////////////Decode gear selector , update inverter and display back onto cluster in car.
      if(frame->id==0x192)
      {
      GLeaver=frame->data.low;
      GLeaver=GLeaver&0x00ffffff; //mask off byte 3
     // Serial.println(GLeaver,HEX);
    switch (GLeaver) {
      case 0x80006a:  //not pressed
        
        break;
      case 0x80506a:  //park button pressed
      gear=PARK;
        shiftPos=0xe1;
        break;
      case 0x800147:  //R position
      gear=REVERSE;
        shiftPos=0xd2;
        break;
      case 0x80042d: //R+ position
      gear=NEUTRAL;
        shiftPos=0xb4; //select Neutral on overpress
        break;
      case 0x800259:  //D pressed
      gear=DRIVE;
            shiftPos=0x78;
        break;
      case 0x800374:  //D+ pressed
      gear=NEUTRAL;
            shiftPos=0xb4; //select Neutral on overpress.
        break;
      case 0x81006a:  //Left Back button pressed
 
        break;
      case 0x82006a:  //Left Front button pressed
 
        break;
      case 0x84006a:  //right Back button pressed
 
        break;

      case 0x88006a:  //right Front button pressed
 
        break;

      case 0xa0006a:  //  S-M-D button pressed
 
        break;        
      default:
      {
        
      }
      }
      }
    }

/////////////////this can id must be sent once at T15 on to fire up the instrument cluster/////////////////////////
void DashOn(){

        outframe.id = 0x332;            // dash on message
        outframe.length = 2;            // Data payload 2 bytes
        outframe.extended = 0;          // standard id
        outframe.rtr=1;                 //No request
        outframe.data.bytes[0]=0x61;  //sets max rpm on tach (temp thing)
        outframe.data.bytes[1]=0x82;  
        Can1.sendFrame(outframe);
  
    
}
////////////////////////////////////////////////////////////////////////////////////////////////////////



    
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


/////////////Send frames every 10ms and send/rexeive inverter control serial data ///////////////////////////////////////

void Frames10MS()
{
  if(timer_Frames10.check())
  {
   
   if(can_status)
   {

  if(abs(mg2_speed)>750)
  {
    RPM=abs(mg2_speed);
  }
  else
  {
    RPM=750;
  }
    
        word RPM_A;// rpm value for E65
        RPM_A=RPM*4;
        outframe.id = 0x0AA;            // Set our transmission address ID
        outframe.length = 8;            // Data payload 8 bytes
        outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
        outframe.rtr=1;                 //No request
        outframe.data.bytes[0]=0x5f;
        outframe.data.bytes[1]=0x59;  
        outframe.data.bytes[2]=0xff;
        outframe.data.bytes[3]=0x00;
        outframe.data.bytes[4]=lowByte(RPM_A);
        outframe.data.bytes[5]=highByte(RPM_A);
        outframe.data.bytes[6]=0x80;
        outframe.data.bytes[7]=0x99;
       


        Can1.sendFrame(outframe); 

   }
  }    
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////Send these frames every 200ms /////////////////////////////////////////
void Frames200MS()
{
  if(timer_Frames200.check())
  {
digitalWrite(13,!digitalRead(13));//blink led every time we fire this interrrupt.

  if(can_status)
  {

///////////////////////////////////////////////////////////////////////////////////////////////////
        outframe.id = 0x1D2;            // current selected gear message
        outframe.length = 5;            // Data payload 5 bytes
        outframe.extended = 0;          // Extended addresses - 0=11-bit 1=29bit
        outframe.rtr=1;                 //No request
        outframe.data.bytes[0]=shiftPos;  //e1=P  78=D  d2=R  b4=N
        outframe.data.bytes[1]=0x0c;  
        outframe.data.bytes[2]=0x8f;
        outframe.data.bytes[3]=Gcount;
        outframe.data.bytes[4]=0xf0;
        Can1.sendFrame(outframe);
        ///////////////////////////
        //Byte 3 is a counter running from 0D through to ED and then back to 0D///
        //////////////////////////////////////////////

         Gcount=Gcount+0x10;
         if (Gcount==0xED)
         {
          Gcount=0x0D;
         }
         
///////////////////////////////////////////////////////////////////////////////////////////////////////////
  }
  }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////





//Metro timer_diag = Metro(700);

void loop() {

  //Get the voltage frmo the ISA shunt
  isa_dc_bus_voltage = Sensor.Voltage3;
  
/*this is a hack to get around the t15 status check */
  if (digitalRead(IN1) || digitalRead(IN2))
  {
    T15Status=true;
    can_status=true;
  }else{
    if (T15Status == true){
      //Display an 'off' message once.
      SerialDEBUG.println("Turning Off.");
    }
    T15Status=false;
    can_status=false;
  }

  /* end of my hack */

  if((T15Status==true) && (dash_status==false)) //fire the dash wake up on T15 set to on but do only once.
  {
    
    DashOn();
    dash_status=true;
    digitalWrite(InvPower,HIGH);  //turn on inverter, oil pump and pas pump
    analogWrite(OilPumpPWM,oil_power);  //set 50% pwm to oil pump at 1khz for testing
    can_status=true;
  }

    if((T15Status==false))
    {
    dash_status=false;
    HV_Off();  //deactivate high voltage system
    analogWrite(OilPumpPWM,0);  //set 0 pwm to shutdown
    can_status=false;
    digitalWrite(InvPower,LOW);  //turn off inverter, oil pump and pas pump
    }
  
  control_inverter();
  Frames10MS();
  Frames200MS();
  if(timer_diag.check())
  {
   if (T15Status==true){
      diag_mth();
    }
  handle_wifi();
  HV_On(); //check if we need hv

// SerialDEBUG.println(ThrotVal);
 //  SerialDEBUG.println(gear);
 // digitalWrite(13,!digitalRead(13));
  }
}
