#include "Watchy.h"

DS3232RTC Watchy::RTC(false); 
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> Watchy::display(GxEPD2_154_D67(CS, DC, RESET, BUSY));

RTC_DATA_ATTR int guiState;
RTC_DATA_ATTR int menuIndex;
RTC_DATA_ATTR BMA423 sensor;
RTC_DATA_ATTR bool WIFI_CONFIGURED;
RTC_DATA_ATTR bool BLE_CONFIGURED;
RTC_DATA_ATTR weatherData currentWeather;
RTC_DATA_ATTR int weatherIntervalCounter = WEATHER_UPDATE_INTERVAL;
RTC_DATA_ATTR bool sleep_mode = false;
RTC_DATA_ATTR uint8_t lastDayGotSteps = 0;
RTC_DATA_ATTR float voltageLastRead = 0;

String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length()-1;

  for(int i=0; i<=maxIndex && found<=index; i++){
    if(data.charAt(i)==separator || i==maxIndex){
        found++;
        strIndex[0] = strIndex[1]+1;
        strIndex[1] = (i == maxIndex) ? i+1 : i;
    }
  }

  return found>index ? data.substring(strIndex[0], strIndex[1]) : "";
}

Watchy::Watchy(){} //constructor

void Watchy::init(String datetime){
    esp_sleep_wakeup_cause_t wakeup_reason;
    wakeup_reason = esp_sleep_get_wakeup_cause(); //get wake up reason
    Wire.begin(SDA, SCL); //init i2c

    switch (wakeup_reason)
    {
        #ifdef ESP_RTC
        case ESP_SLEEP_WAKEUP_TIMER: //ESP Internal RTC
            if(guiState == WATCHFACE_STATE){
                RTC.read(currentTime);
                currentTime.Minute++;
                tmElements_t tm;
                tm.Month = currentTime.Month;
                tm.Day = currentTime.Day;
                tm.Year = currentTime.Year;
                tm.Hour = currentTime.Hour;
                tm.Minute = currentTime.Minute;
                tm.Second = 0;
                time_t t = makeTime(tm);
                RTC.set(t);
                RTC.read(currentTime);
                showWatchFace(true); //partial updates on tick
            }
            break;        
        #endif
        case ESP_SLEEP_WAKEUP_EXT0: //RTC Alarm
            RTC.alarm(ALARM_2); //resets the alarm flag in the RTC
            if(guiState == WATCHFACE_STATE){
                RTC.read(currentTime);
                if(currentTime.Minute % TIME_UPDATE_PERIOD == 0 || currentTime.Minute==0){
                    showWatchFace(true); //partial updates on tick
                }
                if(currentTime.Hour == SLEEP_HOUR && currentTime.Minute == SLEEP_MINUTE){
                    enableSleep();
                }
            }
            break;
        case ESP_SLEEP_WAKEUP_EXT1: //button Press
            if(sleep_mode){
                disableSleep();
                //break;
            }
            handleButtonPress();
            break;
        default: //reset
            #ifndef ESP_RTC
            _rtcConfig(datetime);
            #endif
            _bmaConfig();
            showWatchFace(false); //full update on reset
            break;
    }
    deepSleep();
}

void Watchy::enableSleep(){
    updateFaceNow();
    sleep_mode = true;
    RTC.alarmInterrupt(ALARM_2, false);
    deepSleep();
}

void Watchy::disableSleep(){
    sleep_mode = false;
    RTC.alarmInterrupt(ALARM_2, true);
    RTC.alarm(ALARM_2); //resets the alarm flag in the RTC

    RTC.read(currentTime);
    showWatchFace(false); //full update on wakeup from sleep mode
}

void Watchy::updateFaceNow(){
    RTC.alarm(ALARM_2);
    RTC.read(currentTime);
    showWatchFace(true);
    guiState = WATCHFACE_STATE;
    //deepSleep();
}

uint Watchy::getSteps() {
    uint8_t day = currentTime.Day;
    if (lastDayGotSteps != day){
        if (!sensor.resetStepCounter()){
            /* handle it */
        } else {
            lastDayGotSteps = day;
        }
    }
    return sensor.getCounter();
}

void Watchy::deepSleep(){
  #ifndef ESP_RTC
  esp_sleep_enable_ext0_wakeup(RTC_PIN, 0); //enable deep sleep wake on RTC interrupt
  #endif  
  #ifdef ESP_RTC
  esp_sleep_enable_timer_wakeup(60000000);//change sleep time here, 60 seconds = 60,000,000, 5 mins = 300,000,000
  #endif 
  esp_sleep_enable_ext1_wakeup(BTN_PIN_MASK, ESP_EXT1_WAKEUP_ANY_HIGH); //enable deep sleep wake on button press
  esp_deep_sleep_start();
}

void Watchy::_rtcConfig(String datetime){
    if(WIFI_CONFIGURED){
        display.println("Wifi configured");
        if(WL_CONNECTED == WiFi.waitForConnectResult()){//attempt to connect for 10s
            display.println("Wifi connected");
        }else{//connection failed, time out
            WIFI_CONFIGURED = false;
            //turn off radios
            //disableRadios();
        }
        // turn on wifi
        // connect wifi
        // get time from NTP Server
        // set datetime
        //WiFi.mode(WIFI_ON);
        //NTP
        //WiFi.mode(WIFI_OFF);
    }
    if(!WIFI_CONFIGURED){
        if(connectWiFi()){//attempt to connect for 10s
            WIFI_CONFIGURED = true;
        }else{//connection failed
            WIFI_CONFIGURED = false;
            //turn off radios
            disableRadios();
        }
    }
    if(datetime != NULL){
        const time_t FUDGE(10);//fudge factor to allow for upload time, etc. (seconds, YMMV)
        tmElements_t tm;
        tm.Year = getValue(datetime, ':', 0).toInt() - YEAR_OFFSET;//offset from 1970, since year is stored in uint8_t        
        tm.Month = getValue(datetime, ':', 1).toInt();
        tm.Day = getValue(datetime, ':', 2).toInt();
        tm.Hour = getValue(datetime, ':', 3).toInt();
        tm.Minute = getValue(datetime, ':', 4).toInt();
        tm.Second = getValue(datetime, ':', 5).toInt();

        time_t t = makeTime(tm) + FUDGE;
        RTC.set(t);

    }
    //https://github.com/JChristensen/DS3232RTC
    RTC.squareWave(SQWAVE_NONE); //disable square wave output
    //RTC.set(compileTime()); //set RTC time to compile time
    RTC.setAlarm(ALM2_EVERY_MINUTE, 0, 0, 0, 0); //alarm wakes up Watchy every minute
    //RTC.setAlarm(ALM2_MATCH_MINUTES, 0, 5, 0, 0); //alarm wakes up Watchy at 5 minutes past
    //RTC.setAlarm(ALM2_MATCH_MINUTES, 0, 10, 0, 0); //alarm wakes up Watchy at 10 minutes past
    RTC.alarmInterrupt(ALARM_2, true); //enable alarm interrupt
    RTC.read(currentTime);
}

void Watchy::handleButtonPress(){
  uint64_t wakeupBit = esp_sleep_get_ext1_wakeup_status();
  //Menu Button
  if (wakeupBit & MENU_BTN_MASK){
    if(guiState == WATCHFACE_STATE){//enter menu state if coming from watch face
      showMenu(menuIndex, false);
    }else if(guiState == MAIN_MENU_STATE){//if already in menu, then select menu item
      switch(menuIndex)
      {
        case 0:
          showBattery();
          break;
        case 1:
          getTime();
          //showBuzz();
          break;          
        case 2:
          enableSleep();
          //showAccelerometer();
          break;
        case 3:
          setTime();
          break;
        case 4:
          setupWiFi();
          break;                    
        case 5:
          updateFaceNow();
          //showUpdateFW();
          break;
        default:
          break;                              
      }
    }else if(guiState == FW_UPDATE_STATE){
      //updateFWBegin();
    }
  }
  //Back Button
  else if (wakeupBit & BACK_BTN_MASK){
    if(guiState == MAIN_MENU_STATE){//exit to watch face if already in menu
      RTC.alarm(ALARM_2); //resets the alarm flag in the RTC
      RTC.read(currentTime);
      showWatchFace(false);
    }else if(guiState == APP_STATE){
      showMenu(menuIndex, false);//exit to menu if already in app
    }else if(guiState == FW_UPDATE_STATE){
      showMenu(menuIndex, false);//exit to menu if already in app
    }
  }
  //Up Button
  else if (wakeupBit & UP_BTN_MASK){
    if(guiState == MAIN_MENU_STATE){//increment menu index
      menuIndex--;
      if(menuIndex < 0){
        menuIndex = MENU_LENGTH - 1;
      }    
      showMenu(menuIndex, true);
    } else {
        getTime();
    }
  }
  //Down Button
  else if (wakeupBit & DOWN_BTN_MASK){
    if(guiState == MAIN_MENU_STATE){//decrement menu index
      menuIndex++;
      if(menuIndex > MENU_LENGTH - 1){
        menuIndex = 0;
      }
      showMenu(menuIndex, true);
    } else {
        RTC.alarm(ALARM_2);
        RTC.read(currentTime);
        showWatchFace(true);
    }
  }
  
  /***************** fast menu *****************/
  bool timeout = false;
  long lastTimeout = millis();
  pinMode(MENU_BTN_PIN, INPUT);
  pinMode(BACK_BTN_PIN, INPUT);
  pinMode(UP_BTN_PIN, INPUT);
  pinMode(DOWN_BTN_PIN, INPUT);
  while(!timeout){
      if(millis() - lastTimeout > 5000){
          timeout = true;
      }else{
          if(digitalRead(MENU_BTN_PIN) == 1){
            lastTimeout = millis();  
            if(guiState == MAIN_MENU_STATE){//if already in menu, then select menu item
                switch(menuIndex)
                {
                    case 0:
                    showBattery();
                    break;
                    case 1:
                    getTime();
                    //showBuzz();
                    break;          
                    case 2:
                    enableSleep();
                    //showAccelerometer();
                    break;
                    case 3:
                    setTime();
                    break;
                    case 4:
                    setupWiFi();
                    break;                    
                    case 5:
                    updateFaceNow();
                    //showUpdateFW();
                    break;
                    default:
                    break;                              
                }
            }else if(guiState == FW_UPDATE_STATE){
                //updateFWBegin();
            }
          }else if(digitalRead(BACK_BTN_PIN) == 1){
            lastTimeout = millis();
            if(guiState == MAIN_MENU_STATE){//exit to watch face if already in menu
            RTC.alarm(ALARM_2); //resets the alarm flag in the RTC
            RTC.read(currentTime);
            showWatchFace(false);
            break; //leave loop
            }else if(guiState == APP_STATE){
            showMenu(menuIndex, false);//exit to menu if already in app
            }else if(guiState == FW_UPDATE_STATE){
            showMenu(menuIndex, false);//exit to menu if already in app
            }            
          }else if(digitalRead(UP_BTN_PIN) == 1){
            lastTimeout = millis();
            if(guiState == MAIN_MENU_STATE){//increment menu index
            menuIndex--;
            if(menuIndex < 0){
                menuIndex = MENU_LENGTH - 1;
            }    
            showFastMenu(menuIndex);
            }            
          }else if(digitalRead(DOWN_BTN_PIN) == 1){
            lastTimeout = millis();
            if(guiState == MAIN_MENU_STATE){//decrement menu index
            menuIndex++;
            if(menuIndex > MENU_LENGTH - 1){
                menuIndex = 0;
            }
            showFastMenu(menuIndex);
            }         
          }
      }
  }
  display.hibernate();    
}

void Watchy::showMenu(byte menuIndex, bool partialRefresh){
    display.init(0, false); //_initial_refresh to false to prevent full update on init
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);

    int16_t  x1, y1;
    uint16_t w, h;
    int16_t yPos;

    const char *menuItems[] = {"Check Battery", "Get Time", "Enable Sleep", "Set Time", "Setup WiFi", "Refresh Watchface"};
    for(int i=0; i<MENU_LENGTH; i++){
    yPos = 30+(MENU_HEIGHT*i);
    display.setCursor(0, yPos);
    if(i == menuIndex){
        display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
        display.fillRect(x1-1, y1-10, 200, h+15, GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.println(menuItems[i]);
    }else{
        display.setTextColor(GxEPD_WHITE);
        display.println(menuItems[i]);
    }
    }

    display.display(partialRefresh);
    //display.hibernate();

    guiState = MAIN_MENU_STATE;
}

void Watchy::showFastMenu(byte menuIndex){
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);

    int16_t  x1, y1;
    uint16_t w, h;
    int16_t yPos;

    const char *menuItems[] = {"Check Battery", "Get Time", "Enable Sleep", "Set Time", "Setup WiFi", "Refresh Watchface"};
    for(int i=0; i<MENU_LENGTH; i++){
    yPos = 30+(MENU_HEIGHT*i);
    display.setCursor(0, yPos);
    if(i == menuIndex){
        display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
        display.fillRect(x1-1, y1-10, 200, h+15, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        display.println(menuItems[i]);
    }else{
        display.setTextColor(GxEPD_BLACK);
        display.println(menuItems[i]);
    }   
    }

    display.display(true);

    guiState = MAIN_MENU_STATE;
}

void Watchy::showBattery(){
    display.init(0, false); //_initial_refresh to false to prevent full update on init
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(20, 30);
    display.println("Battery Voltage:");
    float voltage = getBatteryVoltage();
    display.setCursor(70, 80);
    display.print(voltage);
    display.println("V");
    display.display(false); //full refresh
    display.hibernate();

    guiState = APP_STATE;      
}

// vibMotor(500,3);//Vibrate the motor for 500ms (half a second), do this 3 times
void Watchy::vibMotor(uint8_t intervalMs, uint8_t length){
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    bool motorOn = false;
    for(int i=0; i<length; i++){
        motorOn = !motorOn;
        digitalWrite(VIB_MOTOR_PIN, motorOn);
        delay(intervalMs);
    }
}

void Watchy::setTime(){

    guiState = APP_STATE;

    RTC.read(currentTime);

    int8_t minute = currentTime.Minute;
    int8_t hour = currentTime.Hour;
    int8_t day = currentTime.Day;
    int8_t month = currentTime.Month;
    int8_t year = currentTime.Year + YEAR_OFFSET - 2000;

    int8_t setIndex = SET_HOUR;

    int8_t blink = 0;

    pinMode(DOWN_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);
    pinMode(MENU_BTN_PIN, INPUT);
    pinMode(BACK_BTN_PIN, INPUT);

    display.init(0, true); //_initial_refresh to false to prevent full update on init
    display.setFullWindow();

    while(1){

    if(digitalRead(MENU_BTN_PIN) == 1){
        setIndex++;
        if(setIndex > SET_DAY){
        break;
        }
    }
    if(digitalRead(BACK_BTN_PIN) == 1){
        if(setIndex != SET_HOUR){
        setIndex--;
        }
    }

    blink = 1 - blink;

    if(digitalRead(DOWN_BTN_PIN) == 1){
        blink = 1;
        switch(setIndex){
        case SET_HOUR:
            hour == 23 ? (hour = 0) : hour++;
            break;
        case SET_MINUTE:
            minute == 59 ? (minute = 0) : minute++;
            break;
        case SET_YEAR:
            year == 99 ? (year = 20) : year++;
            break;
        case SET_MONTH:
            month == 12 ? (month = 1) : month++;
            break;
        case SET_DAY:
            day == 31 ? (day = 1) : day++;
            break;
        default:
            break;
        }      
    }

    if(digitalRead(UP_BTN_PIN) == 1){
        blink = 1;
        switch(setIndex){
        case SET_HOUR:
            hour == 0 ? (hour = 23) : hour--;
            break;
        case SET_MINUTE:
            minute == 0 ? (minute = 59) : minute--;
            break;
        case SET_YEAR:
            year == 20 ? (year = 99) : year--;
            break;
        case SET_MONTH:
            month == 1 ? (month = 12) : month--;
            break;
        case SET_DAY:
            day == 1 ? (day = 31) : day--;
            break;
        default:
            break;
        }   
    }    

    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&DSEG7_Classic_Bold_53);

    display.setCursor(5, 80);
    if(setIndex == SET_HOUR){//blink hour digits
        display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    if(hour < 10){
        display.print("0");
    }
    display.print(hour);

    display.setTextColor(GxEPD_WHITE);
    display.print(":");

    display.setCursor(108, 80);
    if(setIndex == SET_MINUTE){//blink minute digits
        display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }
    if(minute < 10){
        display.print("0");
    }
    display.print(minute);

    display.setTextColor(GxEPD_WHITE);

    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(45, 150);
    if(setIndex == SET_YEAR){//blink minute digits
        display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }    
    display.print(2000+year);

    display.setTextColor(GxEPD_WHITE);
    display.print("/");

    if(setIndex == SET_MONTH){//blink minute digits
        display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }   
    if(month < 10){
        display.print("0");
    }     
    display.print(month);

    display.setTextColor(GxEPD_WHITE);
    display.print("/");

    if(setIndex == SET_DAY){//blink minute digits
        display.setTextColor(blink ? GxEPD_WHITE : GxEPD_BLACK);
    }       
    if(day < 10){
        display.print("0");
    }     
    display.print(day); 
    display.display(true); //partial refresh
    }

    display.hibernate();

    const time_t FUDGE(10);//fudge factor to allow for upload time, etc. (seconds, YMMV)
    tmElements_t tm;
    tm.Month = month;
    tm.Day = day;
    tm.Year = year + 2000 - YEAR_OFFSET;//offset from 1970, since year is stored in uint8_t
    tm.Hour = hour;
    tm.Minute = minute;
    tm.Second = 0;

    time_t t = makeTime(tm) + FUDGE;
    RTC.set(t);

    showMenu(menuIndex, false);

}

void Watchy::showAccelerometer(){
    display.init(0, true); //_initial_refresh to false to prevent full update on init
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);

    Accel acc;

    long previousMillis = 0;
    long interval = 200;

    guiState = APP_STATE;

    pinMode(BACK_BTN_PIN, INPUT);

    while(1){

    unsigned long currentMillis = millis();

    if(digitalRead(BACK_BTN_PIN) == 1){
        break;
    }

    if(currentMillis - previousMillis > interval){
        previousMillis = currentMillis;
        // Get acceleration data
        bool res = sensor.getAccel(acc);
        uint8_t direction = sensor.getDirection();
        display.fillScreen(GxEPD_BLACK);
        display.setCursor(0, 30);
        if(res == false) {
            display.println("getAccel FAIL");
        }else{
        display.print("  X:"); display.println(acc.x);
        display.print("  Y:"); display.println(acc.y);
        display.print("  Z:"); display.println(acc.z);

        display.setCursor(30, 130);
        switch(direction){
            case DIRECTION_DISP_DOWN:
                display.println("FACE DOWN");
                break;
            case DIRECTION_DISP_UP:
                display.println("FACE UP");
                break;
            case DIRECTION_BOTTOM_EDGE:
                display.println("BOTTOM EDGE");
                break;
            case DIRECTION_TOP_EDGE:
                display.println("TOP EDGE");
                break;
            case DIRECTION_RIGHT_EDGE:
                display.println("RIGHT EDGE");
                break;
            case DIRECTION_LEFT_EDGE:
                display.println("LEFT EDGE");
                break;
            default:
                display.println("ERROR!!!");
                break;
        }

        }
        display.display(true); //full refresh
    }
    }

    showMenu(menuIndex, false);
}

void Watchy::showWatchFace(bool partialRefresh){
  display.init(0, false); //_initial_refresh to false to prevent full update on init
  display.setFullWindow();
  drawWatchFace();
  display.display(partialRefresh); //partial refresh
  display.hibernate();
  guiState = WATCHFACE_STATE;
}

void Watchy::drawWatchFace(){
    display.setFont(&DSEG7_Classic_Bold_53);
    display.setCursor(5, 53+60);
    if(currentTime.Hour < 10){
        display.print("0");
    }
    display.print(currentTime.Hour);
    display.print(":");
    if(currentTime.Minute < 10){
        display.print("0");
    }  
    display.println(currentTime.Minute);
}

void Watchy::disableRadios(){
    WiFi.mode(WIFI_OFF);
    btStop();
}

void Watchy::disconnectWiFi(){
    WIFI_CONFIGURED=false;
    WiFi.disconnect();
    disableRadios();
}

weatherData Watchy::getWeatherData(){
    if(weatherIntervalCounter >= WEATHER_UPDATE_INTERVAL){ //only update if WEATHER_UPDATE_INTERVAL has elapsed i.e. 30 minutes
        if(connectWiFi()){//Use Weather API for live data if WiFi is connected
            HTTPClient http;
            http.setConnectTimeout(3000);//3 second max timeout
            String weatherQueryURL = String(OPENWEATHERMAP_URL) + String(CITY_NAME) + String(",") + String(COUNTRY_CODE) + String("&units=") + String(TEMP_UNIT) + String("&appid=") + String(OPENWEATHERMAP_APIKEY);
            http.begin(weatherQueryURL.c_str());
            int httpResponseCode = http.GET();
            if(httpResponseCode == 200) {
                weatherIntervalCounter = 0;
                String payload = http.getString();
                JSONVar responseObject = JSON.parse(payload);
                currentWeather.temperature = int(responseObject["main"]["temp"]);
                currentWeather.humidity = int(responseObject["main"]["humidity"]);
                currentWeather.weatherConditionCode = int(responseObject["weather"][0]["id"]);
            }else{
                //http error
            }
            http.end();
            //turn off radios
            disableRadios();
        }else{//No WiFi, use RTC Temperature
            uint8_t temperature = RTC.temperature() / 4; //celsius
            if(strcmp(TEMP_UNIT, "imperial") == 0){
                temperature = temperature * 9. / 5. + 32.; //fahrenheit
            }
            currentWeather.temperature = temperature;
            currentWeather.humidity = 0;
            currentWeather.weatherConditionCode = 800;
        }
        //weatherIntervalCounter = 0;
    }else{
        //weatherIntervalCounter++;
        weatherIntervalCounter+=TIME_UPDATE_PERIOD;
    }
    return currentWeather;
}

float Watchy::getBatteryVoltage(){
    return analogRead(ADC_PIN) / 4096.0 * 7.23;
}

void Watchy::isCharging(){
    float voltageCurrentRead = getBatteryVoltage();
    if(voltageCurrentRead > voltageLastRead){
        getTime();
        updateFaceNow();
    }
    voltageLastRead = voltageCurrentRead;
}

uint16_t Watchy::_readRegister(uint8_t address, uint8_t reg, uint8_t *data, uint16_t len)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)address, (uint8_t)len);
    uint8_t i = 0;
    while (Wire.available()) {
        data[i++] = Wire.read();
    }
    return 0;
}

uint16_t Watchy::_writeRegister(uint8_t address, uint8_t reg, uint8_t *data, uint16_t len)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(data, len);
    return (0 !=  Wire.endTransmission());
}

void Watchy::_bmaConfig(){
 
    if (sensor.begin(_readRegister, _writeRegister, delay) == false) {
        //fail to init BMA
        return;
    }

    // Accel parameter structure
    Acfg cfg;
    /*!
        Output data rate in Hz, Optional parameters:
            - BMA4_OUTPUT_DATA_RATE_0_78HZ
            - BMA4_OUTPUT_DATA_RATE_1_56HZ
            - BMA4_OUTPUT_DATA_RATE_3_12HZ
            - BMA4_OUTPUT_DATA_RATE_6_25HZ
            - BMA4_OUTPUT_DATA_RATE_12_5HZ
            - BMA4_OUTPUT_DATA_RATE_25HZ
            - BMA4_OUTPUT_DATA_RATE_50HZ
            - BMA4_OUTPUT_DATA_RATE_100HZ
            - BMA4_OUTPUT_DATA_RATE_200HZ
            - BMA4_OUTPUT_DATA_RATE_400HZ
            - BMA4_OUTPUT_DATA_RATE_800HZ
            - BMA4_OUTPUT_DATA_RATE_1600HZ
    */
    cfg.odr = BMA4_OUTPUT_DATA_RATE_100HZ;
    /*!
        G-range, Optional parameters:
            - BMA4_ACCEL_RANGE_2G
            - BMA4_ACCEL_RANGE_4G
            - BMA4_ACCEL_RANGE_8G
            - BMA4_ACCEL_RANGE_16G
    */
    cfg.range = BMA4_ACCEL_RANGE_2G;
    /*!
        Bandwidth parameter, determines filter configuration, Optional parameters:
            - BMA4_ACCEL_OSR4_AVG1
            - BMA4_ACCEL_OSR2_AVG2
            - BMA4_ACCEL_NORMAL_AVG4
            - BMA4_ACCEL_CIC_AVG8
            - BMA4_ACCEL_RES_AVG16
            - BMA4_ACCEL_RES_AVG32
            - BMA4_ACCEL_RES_AVG64
            - BMA4_ACCEL_RES_AVG128
    */
    cfg.bandwidth = BMA4_ACCEL_NORMAL_AVG4;

    /*! Filter performance mode , Optional parameters:
        - BMA4_CIC_AVG_MODE
        - BMA4_CONTINUOUS_MODE
    */
    cfg.perf_mode = BMA4_CONTINUOUS_MODE;

    // Configure the BMA423 accelerometer
    sensor.setAccelConfig(cfg);

    // Enable BMA423 accelerometer
    // Warning : Need to use feature, you must first enable the accelerometer
    sensor.enableAccel();

    struct bma4_int_pin_config config ;
    config.edge_ctrl = BMA4_LEVEL_TRIGGER;
    config.lvl = BMA4_ACTIVE_HIGH;
    config.od = BMA4_PUSH_PULL;
    config.output_en = BMA4_OUTPUT_ENABLE;
    config.input_en = BMA4_INPUT_DISABLE;
    // The correct trigger interrupt needs to be configured as needed
    sensor.setINTPinConfig(config, BMA4_INTR1_MAP);

    struct bma423_axes_remap remap_data;
    remap_data.x_axis = 1;
    remap_data.x_axis_sign = 0xFF;
    remap_data.y_axis = 0;
    remap_data.y_axis_sign = 0xFF;
    remap_data.z_axis = 2;
    remap_data.z_axis_sign = 0xFF;
    // Need to raise the wrist function, need to set the correct axis
    sensor.setRemapAxes(&remap_data);

    // Enable BMA423 isStepCounter feature
    sensor.enableFeature(BMA423_STEP_CNTR, true);
    // Enable BMA423 isTilt feature
    sensor.enableFeature(BMA423_TILT, true);
    // Enable BMA423 isDoubleClick feature
    sensor.enableFeature(BMA423_WAKEUP, true);

    // Reset steps
    sensor.resetStepCounter();

    // Turn on feature interrupt
    sensor.enableStepCountInterrupt();
    sensor.enableTiltInterrupt();
    // It corresponds to isDoubleClick interrupt
    sensor.enableWakeupInterrupt();  
}

bool Watchy::setupWiFi(){
    //
    WiFiMulti wifiMulti;
    //Serial.begin(115200);
    //delay(10);

    wifiMulti.addAP(WIFI_SSID_HOME, WIFI_PASS_HOME);
    wifiMulti.addAP(WIFI_SSID_WORK, WIFI_PASS_WORK);
    wifiMulti.addAP(WIFI_SSID_PHONE, WIFI_PASS_PHONE);

    display.init(0, false);//_initial_refresh
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(0, 30);

    display.println("Connecting Wifi...");
    if(wifiMulti.run() == WL_CONNECTED) {
        WIFI_CONFIGURED = true;

        display.println("Connected to");
        display.println(WiFi.SSID());

        display.println("");
        display.println("WiFi connected");
        display.println("IP address: ");
        display.println(WiFi.localIP());
        display.display(false);//full refresh
        display.hibernate();
    } else {
        WIFI_CONFIGURED = false;
        display.println("Connection failed");
        disableRadios();
    }
    return WIFI_CONFIGURED;
}

bool Watchy::connectWiFi(){
    WiFiMulti wifiMulti;

    wifiMulti.addAP(WIFI_SSID_HOME, WIFI_PASS_HOME);
    wifiMulti.addAP(WIFI_SSID_WORK, WIFI_PASS_WORK);
    wifiMulti.addAP(WIFI_SSID_PHONE, WIFI_PASS_PHONE);

    if(wifiMulti.run() == WL_CONNECTED) {
        WIFI_CONFIGURED = true;
    } else {
        WIFI_CONFIGURED = false;
        disableRadios();
    }
    return WIFI_CONFIGURED;
}

void Watchy::getTime(){
    display.println("Connecting to WIFI");
    //if(connectWiFi()){
    if(connectWiFi()){
        display.init(0, false); //_initial_refresh to false to prevent full update on init
        display.setFullWindow();
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(0, 30);
        display.println("Checking NTP time");

        configTzTime(TIMEZONE_STRING,NTP_SERVER_1,NTP_SERVER_1,NTP_SERVER_3);
        int i = 0;
        while (time(nullptr) < 1000000000l && i < 40) {
            delay(500);
            i++;
        }
        time_t tnow = time(nullptr);
        struct tm *local = localtime(&tnow);

        display.println("NTP Retrieved");
        display.print("Date: ");
        display.print(local->tm_year + 1900);
        display.print("-");
        if((local->tm_mon + 1) < 10){
            display.print("0");
        }
        display.print(local->tm_mon + 1);
        display.print("-");
        if(local->tm_mday < 10){
            display.print("0");
        }
        display.println(local->tm_mday);
        display.print("Time: ");
        if(local->tm_hour < 10){
            display.print("0");
        }
        display.print(local->tm_hour);
        display.print(":");
        if(local->tm_min < 10){
            display.print("0");
        }
        display.print(local->tm_min);
        display.print(":");
        if(local->tm_sec < 10){
            display.print("0");
        }
        display.println(local->tm_sec);
        display.print("Week Day: ");
        display.print(local->tm_wday);
        display.display(false); //full refresh
        display.hibernate();

        currentTime.Year = local->tm_year + 1900 - 18;
        currentTime.Month = local->tm_mon + 1;
        currentTime.Day = local->tm_mday;
        currentTime.Hour = local->tm_hour;
        currentTime.Minute = local->tm_min;
        currentTime.Second = local->tm_sec;
        currentTime.Wday = local->tm_wday+1;
        RTC.write(currentTime);
        disableRadios();
    } else {
        display.println("Connection failed");
        //disableRadios();
    }
    disableRadios();
}

void Watchy::drawHelperGrid(){
    display.fillScreen(GxEPD_WHITE);
    for(int i=0; i<=200; i+=20){
        display.drawLine(i,0,i,200,GxEPD_BLACK);
        display.drawLine(0,i,200,i,GxEPD_BLACK);
    }
}