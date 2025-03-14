#include "selfdrive/boardd/boardd.h"

#include <sched.h>
#include <sys/cdefs.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

#include "cereal/gen/cpp/car.capnp.h"
#include "cereal/messaging/messaging.h"
#include "common/params.h"
#include "common/swaglog.h"
#include "common/timing.h"
#include "common/util.h"
#include "system/hardware/hw.h"

// for panda gps
#ifdef QCOM
#include <libusb-1.0/libusb.h>
#include "selfdrive/boardd/pigeon.h"
#endif
// -- Multi-panda conventions --
// Ordering:
// - The internal panda will always be the first panda
// - Consecutive pandas will be sorted based on panda type, and then serial number
// Connecting:
// - If a panda connection is dropped, boardd will reconnect to all pandas
// - If a panda is added, we will only reconnect when we are offroad
// CAN buses:
// - Each panda will have it's block of 4 buses. E.g.: the second panda will use
//   bus numbers 4, 5, 6 and 7
// - The internal panda will always be used for accessing the OBD2 port,
//   and thus firmware queries
// Safety:
// - SafetyConfig is a list, which is mapped to the connected pandas
// - If there are more pandas connected than there are SafetyConfigs,
//   the excess pandas will remain in "silent" or "noOutput" mode
// Ignition:
// - If any of the ignition sources in any panda is high, ignition is high

#define MAX_IR_POWER 0.5f
#define MIN_IR_POWER 0.0f
#ifdef QCOM
#define CUTOFF_IL 200
#define SATURATE_IL 1600
#else
#define CUTOFF_IL 400
#define SATURATE_IL 1000
#endif
#define NIBBLE_TO_HEX(n) ((n) < 10 ? (n) + '0' : ((n) - 10) + 'a')
using namespace std::chrono_literals;

std::atomic<bool> ignition(false);
#ifdef QCOM
std::atomic<bool> pigeon_active(false);
#endif
ExitHandler do_exit;

static std::string get_time_str(const struct tm &time) {
  char s[30] = {'\0'};
  std::strftime(s, std::size(s), "%Y-%m-%d %H:%M:%S", &time);
  return s;
}

bool check_all_connected(const std::vector<Panda *> &pandas) {
  for (const auto& panda : pandas) {
    if (!panda->connected()) {
      do_exit = true;
      return false;
    }
  }
  return true;
}

enum class SyncTimeDir { TO_PANDA, FROM_PANDA };

void sync_time(Panda *panda, SyncTimeDir dir) {
  if (!panda->has_rtc) return;

  setenv("TZ", "UTC", 1);
  struct tm sys_time = util::get_time();
  struct tm rtc_time = panda->get_rtc();

  if (dir == SyncTimeDir::TO_PANDA) {
    if (util::time_valid(sys_time)) {
      // Write time to RTC if it looks reasonable
      double seconds = difftime(mktime(&rtc_time), mktime(&sys_time));
      if (std::abs(seconds) > 1.1) {
        panda->set_rtc(sys_time);
        LOGW("Updating panda RTC. dt = %.2f System: %s RTC: %s",
              seconds, get_time_str(sys_time).c_str(), get_time_str(rtc_time).c_str());
      }
    }
  } else if (dir == SyncTimeDir::FROM_PANDA) {
    LOGW("System time: %s, RTC time: %s", get_time_str(sys_time).c_str(), get_time_str(rtc_time).c_str());

    if (!util::time_valid(sys_time) && util::time_valid(rtc_time)) {
      const struct timeval tv = {mktime(&rtc_time), 0};
      settimeofday(&tv, 0);
      LOGE("System time wrong, setting from RTC.");
    }
  }
}

bool safety_setter_thread(std::vector<Panda *> pandas) {
  LOGD("Starting safety setter thread");

  Params p;

  // there should be at least one panda connected
  if (pandas.size() == 0) {
    return false;
  }

  // initialize to ELM327 without OBD multiplexing for fingerprinting
  bool obd_multiplexing_enabled = false;
  for (int i = 0; i < pandas.size(); i++) {
    pandas[i]->set_safety_model(cereal::CarParams::SafetyModel::ELM327, 1U);
  }

  // openpilot can switch between multiplexing modes for different FW queries
  while (true) {
    if (do_exit || !check_all_connected(pandas) || !ignition) {
      return false;
    }

    bool obd_multiplexing_requested = p.getBool("ObdMultiplexingEnabled");
    if (obd_multiplexing_requested != obd_multiplexing_enabled) {
      for (int i = 0; i < pandas.size(); i++) {
        const uint16_t safety_param = (i > 0 || !obd_multiplexing_requested) ? 1U : 0U;
        pandas[i]->set_safety_model(cereal::CarParams::SafetyModel::ELM327, safety_param);
      }
      obd_multiplexing_enabled = obd_multiplexing_requested;
      p.putBool("ObdMultiplexingChanged", true);
    }

    if (p.getBool("FirmwareQueryDone")) {
      LOGW("finished FW query");
      break;
    }
    util::sleep_for(20);
  }

  std::string params;
  LOGW("waiting for params to set safety model");
  while (true) {
    if (do_exit || !check_all_connected(pandas) || !ignition) {
      return false;
    }

    if (p.getBool("ControlsReady")) {
      params = p.get("CarParams");
      if (params.size() > 0) break;
    }
    util::sleep_for(100);
  }
  LOGW("got %d bytes CarParams", params.size());

  AlignedBuffer aligned_buf;
  capnp::FlatArrayMessageReader cmsg(aligned_buf.align(params.data(), params.size()));
  cereal::CarParams::Reader car_params = cmsg.getRoot<cereal::CarParams>();
  cereal::CarParams::SafetyModel safety_model;
  uint16_t safety_param;

  auto safety_configs = car_params.getSafetyConfigs();
  uint16_t alternative_experience = car_params.getAlternativeExperience();
  for (uint32_t i = 0; i < pandas.size(); i++) {
    auto panda = pandas[i];

    if (safety_configs.size() > i) {
      safety_model = safety_configs[i].getSafetyModel();
      safety_param = safety_configs[i].getSafetyParam();
    } else {
      // If no safety mode is specified, default to silent
      safety_model = cereal::CarParams::SafetyModel::SILENT;
      safety_param = 0U;
    }

    LOGW("panda %d: setting safety model: %d, param: %d, alternative experience: %d", i, (int)safety_model, safety_param, alternative_experience);
    panda->set_alternative_experience(alternative_experience);
    panda->set_safety_model(safety_model, safety_param);
  }

  return true;
}

Panda *connect(std::string serial="", uint32_t index=0) {
  std::unique_ptr<Panda> panda;
  try {
    panda = std::make_unique<Panda>(serial, (index * PANDA_BUS_CNT));
  } catch (std::exception &e) {
    return nullptr;
  }

  // common panda config
  if (getenv("BOARDD_LOOPBACK")) {
    panda->set_loopback(true);
  }
  //panda->enable_deepsleep();

#ifdef QCOM
  // power on charging, only the first time. Panda can also change mode and it causes a brief disconneciton
#ifndef __x86_64__
  static std::once_flag connected_once;
  std::call_once(connected_once, &Panda::set_usb_power_mode, panda, cereal::PeripheralState::UsbPowerMode::CDP);
#endif
#endif
  sync_time(panda.get(), SyncTimeDir::FROM_PANDA);
  return panda.release();
}

void can_send_thread(std::vector<Panda *> pandas, bool fake_send) {
  util::set_thread_name("boardd_can_send");

  AlignedBuffer aligned_buf;
  std::unique_ptr<Context> context(Context::create());
  std::unique_ptr<SubSocket> subscriber(SubSocket::create(context.get(), "sendcan"));
  assert(subscriber != NULL);
  subscriber->setTimeout(100);

  // run as fast as messages come in
  while (!do_exit && check_all_connected(pandas)) {
    std::unique_ptr<Message> msg(subscriber->receive());
    if (!msg) {
      if (errno == EINTR) {
        do_exit = true;
      }
      continue;
    }

    capnp::FlatArrayMessageReader cmsg(aligned_buf.align(msg.get()));
    cereal::Event::Reader event = cmsg.getRoot<cereal::Event>();

    //Dont send if older than 1 second
    if ((nanos_since_boot() - event.getLogMonoTime() < 1e9) && !fake_send) {
      for (const auto& panda : pandas) {
        LOGT("sending sendcan to panda: %s", (panda->hw_serial()).c_str());
        panda->can_send(event.getSendcan());
        LOGT("sendcan sent to panda: %s", (panda->hw_serial()).c_str());
      }
    } else {
      LOGE("sendcan too old to send: %llu, %llu", nanos_since_boot(), event.getLogMonoTime());
    }
  }
}

void can_recv_thread(std::vector<Panda *> pandas) {
  util::set_thread_name("boardd_can_recv");

  // can = 8006
  PubMaster pm({"can"});

  // run at 100hz
  const uint64_t dt = 10000000ULL;
  uint64_t next_frame_time = nanos_since_boot() + dt;
  std::vector<can_frame> raw_can_data;

  while (!do_exit && check_all_connected(pandas)) {
    bool comms_healthy = true;
    raw_can_data.clear();
    for (const auto& panda : pandas) {
      comms_healthy &= panda->can_receive(raw_can_data);
    }

    MessageBuilder msg;
    auto evt = msg.initEvent();
    evt.setValid(comms_healthy);
    auto canData = evt.initCan(raw_can_data.size());
    for (uint i = 0; i<raw_can_data.size(); i++) {
      canData[i].setAddress(raw_can_data[i].address);
      canData[i].setBusTime(raw_can_data[i].busTime);
      canData[i].setDat(kj::arrayPtr((uint8_t*)raw_can_data[i].dat.data(), raw_can_data[i].dat.size()));
      canData[i].setSrc(raw_can_data[i].src);
    }
    pm.send("can", msg);

    uint64_t cur_time = nanos_since_boot();
    int64_t remaining = next_frame_time - cur_time;
    if (remaining > 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
    } else {
      if (ignition) {
        LOGW("missed cycles (%d) %lld", (int)-1*remaining/dt, remaining);
      }
      next_frame_time = cur_time;
    }

    next_frame_time += dt;
  }
}

void send_empty_peripheral_state(PubMaster *pm) {
  MessageBuilder msg;
  auto peripheralState  = msg.initEvent().initPeripheralState();
  peripheralState.setPandaType(cereal::PandaState::PandaType::UNKNOWN);
  pm->send("peripheralState", msg);
}

void send_empty_panda_state(PubMaster *pm) {
  MessageBuilder msg;
  auto pandaStates = msg.initEvent().initPandaStates(1);
  pandaStates[0].setPandaType(cereal::PandaState::PandaType::UNKNOWN);
  pm->send("pandaStates", msg);
}

std::optional<bool> send_panda_states(PubMaster *pm, const std::vector<Panda *> &pandas, bool spoofing_started) {
  bool ignition_local = false;
  const uint32_t pandas_cnt = pandas.size();

  // build msg
  MessageBuilder msg;
  auto evt = msg.initEvent();
  auto pss = evt.initPandaStates(pandas_cnt);

  std::vector<health_t> pandaStates;
  pandaStates.reserve(pandas_cnt);

  #ifndef QCOM
  std::vector<std::array<can_health_t, PANDA_CAN_CNT>> pandaCanStates;
  pandaCanStates.reserve(pandas_cnt);
  #endif

  for (const auto& panda : pandas){
    auto health_opt = panda->get_state();
    if (!health_opt) {
      return std::nullopt;
    }

    health_t health = *health_opt;

    #ifndef QCOM
    std::array<can_health_t, PANDA_CAN_CNT> can_health{};
    for (uint32_t i = 0; i < PANDA_CAN_CNT; i++) {
      auto can_health_opt = panda->get_can_state(i);
      if (!can_health_opt) {
        return std::nullopt;
      }
      can_health[i] = *can_health_opt;
    }
    pandaCanStates.push_back(can_health);
    #endif

    if (spoofing_started) {
      health.ignition_line_pkt = 1;
    }

    ignition_local |= ((health.ignition_line_pkt != 0) || (health.ignition_can_pkt != 0));

    pandaStates.push_back(health);
  }

  for (uint32_t i = 0; i < pandas_cnt; i++) {
    auto panda = pandas[i];
    const auto &health = pandaStates[i];

    // Make sure CAN buses are live: safety_setter_thread does not work if Panda CAN are silent and there is only one other CAN node
    if (health.safety_mode_pkt == (uint8_t)(cereal::CarParams::SafetyModel::SILENT)) {
      panda->set_safety_model(cereal::CarParams::SafetyModel::NO_OUTPUT);
    }

  #ifndef __x86_64__
    bool power_save_desired = !ignition_local;
    #ifdef QCOM
    power_save_desired = power_save_desired && !pigeon_active;
    #endif
    if (health.power_save_enabled_pkt != power_save_desired) {
      panda->set_power_saving(power_save_desired);
    }

    // set safety mode to NO_OUTPUT when car is off. ELM327 is an alternative if we want to leverage athenad/connect
    if (!ignition_local && (health.safety_mode_pkt != (uint8_t)(cereal::CarParams::SafetyModel::NO_OUTPUT))) {
      panda->set_safety_model(cereal::CarParams::SafetyModel::NO_OUTPUT);
    }
  #endif

    if (!panda->comms_healthy()) {
      evt.setValid(false);
    }

    auto ps = pss[i];
    #ifndef QCOM
    ps.setVoltage(health.voltage_pkt);
    ps.setCurrent(health.current_pkt);
    #endif
    ps.setUptime(health.uptime_pkt);
    ps.setSafetyTxBlocked(health.safety_tx_blocked_pkt);
    ps.setSafetyRxInvalid(health.safety_rx_invalid_pkt);
    ps.setIgnitionLine(health.ignition_line_pkt);
    ps.setIgnitionCan(health.ignition_can_pkt);
    ps.setControlsAllowed(health.controls_allowed_pkt);
    ps.setGasInterceptorDetected(health.gas_interceptor_detected_pkt);
    ps.setTxBufferOverflow(health.tx_buffer_overflow_pkt);
    ps.setRxBufferOverflow(health.rx_buffer_overflow_pkt);
    ps.setGmlanSendErrs(health.gmlan_send_errs_pkt);
    ps.setPandaType(panda->hw_type);
    ps.setSafetyModel(cereal::CarParams::SafetyModel(health.safety_mode_pkt));
    ps.setSafetyParam(health.safety_param_pkt);
    ps.setFaultStatus(cereal::PandaState::FaultStatus(health.fault_status_pkt));
    ps.setPowerSaveEnabled((bool)(health.power_save_enabled_pkt));
    ps.setHeartbeatLost((bool)(health.heartbeat_lost_pkt));
    ps.setAlternativeExperience(health.alternative_experience_pkt);
    ps.setHarnessStatus(cereal::PandaState::HarnessStatus(health.car_harness_status_pkt));
    ps.setInterruptLoad(health.interrupt_load);
    ps.setFanPower(health.fan_power);
    #ifndef QCOM
    ps.setFanStallCount(health.fan_stall_count);
    #endif
    ps.setSafetyRxChecksInvalid((bool)(health.safety_rx_checks_invalid));
    #ifndef QCOM
    ps.setSpiChecksumErrorCount(health.spi_checksum_error_count);
    ps.setSbu1Voltage(health.sbu1_voltage_mV / 1000.0f);
    ps.setSbu2Voltage(health.sbu2_voltage_mV / 1000.0f);

    std::array<cereal::PandaState::PandaCanState::Builder, PANDA_CAN_CNT> cs = {ps.initCanState0(), ps.initCanState1(), ps.initCanState2()};

    for (uint32_t j = 0; j < PANDA_CAN_CNT; j++) {
      const auto &can_health = pandaCanStates[i][j];
      cs[j].setBusOff((bool)can_health.bus_off);
      cs[j].setBusOffCnt(can_health.bus_off_cnt);
      cs[j].setErrorWarning((bool)can_health.error_warning);
      cs[j].setErrorPassive((bool)can_health.error_passive);
      cs[j].setLastError(cereal::PandaState::PandaCanState::LecErrorCode(can_health.last_error));
      cs[j].setLastStoredError(cereal::PandaState::PandaCanState::LecErrorCode(can_health.last_stored_error));
      cs[j].setLastDataError(cereal::PandaState::PandaCanState::LecErrorCode(can_health.last_data_error));
      cs[j].setLastDataStoredError(cereal::PandaState::PandaCanState::LecErrorCode(can_health.last_data_stored_error));
      cs[j].setReceiveErrorCnt(can_health.receive_error_cnt);
      cs[j].setTransmitErrorCnt(can_health.transmit_error_cnt);
      cs[j].setTotalErrorCnt(can_health.total_error_cnt);
      cs[j].setTotalTxLostCnt(can_health.total_tx_lost_cnt);
      cs[j].setTotalRxLostCnt(can_health.total_rx_lost_cnt);
      cs[j].setTotalTxCnt(can_health.total_tx_cnt);
      cs[j].setTotalRxCnt(can_health.total_rx_cnt);
      cs[j].setTotalFwdCnt(can_health.total_fwd_cnt);
      cs[j].setCanSpeed(can_health.can_speed);
      cs[j].setCanDataSpeed(can_health.can_data_speed);
      cs[j].setCanfdEnabled(can_health.canfd_enabled);
      cs[j].setBrsEnabled(can_health.brs_enabled);
      cs[j].setCanfdNonIso(can_health.canfd_non_iso);
    }
    #endif

    // Convert faults bitset to capnp list
    std::bitset<sizeof(health.faults_pkt) * 8> fault_bits(health.faults_pkt);
    auto faults = ps.initFaults(fault_bits.count());

    size_t j = 0;
    for (size_t f = size_t(cereal::PandaState::FaultType::RELAY_MALFUNCTION);
         f <= size_t(cereal::PandaState::FaultType::INTERRUPT_RATE_EXTI); f++) {
      if (fault_bits.test(f)) {
        faults.set(j, cereal::PandaState::FaultType(f));
        j++;
      }
    }
  }

  pm->send("pandaStates", msg);
  return ignition_local;
}

void send_peripheral_state(PubMaster *pm, Panda *panda) {
  #ifdef QCOM
  auto pandaState_opt = panda->get_state();
  if (!pandaState_opt) {
    return;
  }

  health_t pandaState = *pandaState_opt;
  #endif
  // build msg
  MessageBuilder msg;
  auto evt = msg.initEvent();
  evt.setValid(panda->comms_healthy());

  auto ps = evt.initPeripheralState();
  ps.setPandaType(panda->hw_type);

  #ifdef QCOM
  ps.setVoltage(pandaState.voltage_pkt);
  ps.setCurrent(pandaState.current_pkt);
  #else
  double read_time = millis_since_boot();
  ps.setVoltage(Hardware::get_voltage());
  ps.setCurrent(Hardware::get_current());
  read_time = millis_since_boot() - read_time;
  if (read_time > 50) {
    LOGW("reading hwmon took %lfms", read_time);
  }
  #endif

  uint16_t fan_speed_rpm = panda->get_fan_speed();
  #ifdef QCOM
  ps.setUsbPowerMode(cereal::PeripheralState::UsbPowerMode(pandaState.usb_power_mode_pkt));
  #endif
  ps.setFanSpeedRpm(fan_speed_rpm);

  pm->send("peripheralState", msg);
}

void panda_state_thread(PubMaster *pm, std::vector<Panda *> pandas, bool spoofing_started) {
  util::set_thread_name("boardd_panda_state");

  Params params;
  SubMaster sm({"controlsState"});

  Panda *peripheral_panda = pandas[0];
  bool is_onroad = false;
  bool is_onroad_last = false;
  std::future<bool> safety_future;

  std::vector<std::string> connected_serials;
  for (Panda *p : pandas) {
    connected_serials.push_back(p->hw_serial());
  }

  LOGD("start panda state thread");

  // run at 2hz
  while (!do_exit && check_all_connected(pandas)) {
    uint64_t start_time = nanos_since_boot();

    // send out peripheralState
    send_peripheral_state(pm, peripheral_panda);
    auto ignition_opt = send_panda_states(pm, pandas, spoofing_started);

    if (!ignition_opt) {
      continue;
    }

    ignition = *ignition_opt;

    // check if we should have pandad reconnect
    if (!ignition) {
      bool comms_healthy = true;
      for (const auto &panda : pandas) {
        comms_healthy &= panda->comms_healthy();
      }

      if (!comms_healthy) {
        LOGE("Reconnecting, communication to pandas not healthy");
        do_exit = true;

      } else {
        // check for new pandas
        for (std::string &s : Panda::list(true)) {
          if (!std::count(connected_serials.begin(), connected_serials.end(), s)) {
            LOGW("Reconnecting to new panda: %s", s.c_str());
            do_exit = true;
            break;
          }
        }
      }

      if (do_exit) {
        break;
      }
    }

    is_onroad = params.getBool("IsOnroad");

    // set new safety on onroad transition, after params are cleared
    if (is_onroad && !is_onroad_last) {
      if (!safety_future.valid() || safety_future.wait_for(0ms) == std::future_status::ready) {
        safety_future = std::async(std::launch::async, safety_setter_thread, pandas);
      } else {
        LOGW("Safety setter thread already running");
      }
    }

    is_onroad_last = is_onroad;

    sm.update(0);
    const bool engaged = sm.allAliveAndValid({"controlsState"}) && sm["controlsState"].getControlsState().getEnabled();

    for (const auto &panda : pandas) {
      panda->send_heartbeat(engaged);
    }

    uint64_t dt = nanos_since_boot() - start_time;
    util::sleep_for(500 - dt / 1000000ULL);
  }
}


void peripheral_control_thread(Panda *panda, bool no_fan_control) {
  util::set_thread_name("boardd_peripheral_control");
  // rick - a device with black panda = EON / LEON / clone 1.5
  if (!Params().getBool("dp_no_fan_ctrl")) {
    no_fan_control = panda->hw_type == cereal::PandaState::PandaType::BLACK_PANDA;
    Params().putBool("dp_no_fan_ctrl", no_fan_control);
  }

  SubMaster sm({"deviceState", "driverCameraState"});

  uint64_t last_front_frame_t = 0;
  uint16_t prev_fan_speed = 999;
  uint16_t ir_pwr = 0;
  uint16_t prev_ir_pwr = 999;
  unsigned int cnt = 0;
  #ifdef QCOM
  bool prev_charging_disabled = false;
  #endif

  FirstOrderFilter integ_lines_filter(0, 30.0, 0.05);

  while (!do_exit && panda->connected()) {
    cnt++;
    sm.update(1000); // TODO: what happens if EINTR is sent while in sm.update?
    #ifdef QCOM
    if (!Hardware::PC() && sm.updated("deviceState")) {
      // Charging mode
      bool charging_disabled = sm["deviceState"].getDeviceState().getChargingDisabled();
      if (charging_disabled != prev_charging_disabled) {
        if (charging_disabled) {
          panda->set_usb_power_mode(cereal::PeripheralState::UsbPowerMode::CLIENT);
          LOGW("TURN OFF CHARGING!\n");
        } else {
          panda->set_usb_power_mode(cereal::PeripheralState::UsbPowerMode::CDP);
          LOGW("TURN ON CHARGING!\n");
        }
        prev_charging_disabled = charging_disabled;
      }
    }

    // Other pandas don't have fan/IR to control
    if (panda->hw_type != cereal::PandaState::PandaType::UNO && panda->hw_type != cereal::PandaState::PandaType::DOS) continue;
    #endif
    if (sm.updated("deviceState") && !no_fan_control) {
      // Fan speed
      uint16_t fan_speed = sm["deviceState"].getDeviceState().getFanSpeedPercentDesired();
      if (fan_speed != prev_fan_speed || cnt % 100 == 0) {
        panda->set_fan_speed(fan_speed);
        prev_fan_speed = fan_speed;
      }
    }

    if (sm.updated("driverCameraState")) {
      auto event = sm["driverCameraState"];
      int cur_integ_lines = event.getDriverCameraState().getIntegLines();

      #ifndef QCOM
      cur_integ_lines = integ_lines_filter.update(cur_integ_lines);
      #endif
      last_front_frame_t = event.getLogMonoTime();

      if (cur_integ_lines <= CUTOFF_IL) {
        ir_pwr = 100.0 * MIN_IR_POWER;
      } else if (cur_integ_lines > SATURATE_IL) {
        ir_pwr = 100.0 * MAX_IR_POWER;
      } else {
        ir_pwr = 100.0 * (MIN_IR_POWER + ((cur_integ_lines - CUTOFF_IL) * (MAX_IR_POWER - MIN_IR_POWER) / (SATURATE_IL - CUTOFF_IL)));
      }
    }

    // Disable ir_pwr on front frame timeout
    uint64_t cur_t = nanos_since_boot();
    if (cur_t - last_front_frame_t > 1e9) {
      ir_pwr = 0;
    }

    if (ir_pwr != prev_ir_pwr || cnt % 100 == 0 || ir_pwr >= 50.0) {
      panda->set_ir_pwr(ir_pwr);
      prev_ir_pwr = ir_pwr;
    }

    // Write to rtc once per minute when no ignition present
    if (!ignition && (cnt % 120 == 1)) {
      sync_time(panda, SyncTimeDir::TO_PANDA);
    }
  }
}
#ifdef QCOM
static void pigeon_publish_raw(PubMaster &pm, const std::string &dat) {
  // create message
  MessageBuilder msg;
  msg.initEvent().setUbloxRaw(capnp::Data::Reader((uint8_t*)dat.data(), dat.length()));
  pm.send("ubloxRaw", msg);
}

void pigeon_thread(Panda *panda) {
  Params().putBool("dp_no_gps_ctrl", !panda->has_gps);
  if (!panda->has_gps) return;
  util::set_thread_name("boardd_pigeon");

  PubMaster pm({"ubloxRaw"});
  bool ignition_last = false;

  std::unique_ptr<Pigeon> pigeon(Hardware::TICI() ? Pigeon::connect("/dev/ttyHS0") : Pigeon::connect(panda));

  while (!do_exit && panda->connected()) {
    bool need_reset = false;
    bool ignition_local = ignition;
    std::string recv = pigeon->receive();

    // Check based on null bytes
    if (ignition_local && recv.length() > 0 && recv[0] == (char)0x00) {
      need_reset = true;
      LOGW("received invalid ublox message while onroad, resetting panda GPS");
    }

    if (recv.length() > 0) {
      pigeon_publish_raw(pm, recv);
    }

    // init pigeon on rising ignition edge
    // since it was turned off in low power mode
    if((ignition_local && !ignition_last) || need_reset) {
      pigeon_active = true;
      pigeon->init();
    } else if (!ignition_local && ignition_last) {
      // power off on falling edge of ignition
      LOGD("powering off pigeon\n");
      pigeon->stop();
      pigeon->set_power(false);
      pigeon_active = false;
    }

    ignition_last = ignition_local;

    // 10ms - 100 Hz
    util::sleep_for(10);
  }
}
#endif
void boardd_main_thread(std::vector<std::string> serials) {
  PubMaster pm({"pandaStates", "peripheralState"});
  LOGW("attempting to connect");

  if (serials.size() == 0) {
    // connect to all
    serials = Panda::list();

    // exit if no pandas are connected
    if (serials.size() == 0) {
      LOGW("no pandas found, exiting");
      return;
    }
  }

  // connect to all provided serials
  std::vector<Panda *> pandas;
  for (int i = 0; i < serials.size() && !do_exit; /**/) {
    Panda *p = connect(serials[i], i);
    if (!p) {
      // send empty pandaState & peripheralState and try again
      send_empty_panda_state(&pm);
      send_empty_peripheral_state(&pm);
      util::sleep_for(500);
      continue;
    }

    pandas.push_back(p);
    ++i;
  }

  if (!do_exit) {
    LOGW("connected to board");
    Panda *peripheral_panda = pandas[0];
    std::vector<std::thread> threads;

    threads.emplace_back(panda_state_thread, &pm, pandas, getenv("STARTED") != nullptr);
    threads.emplace_back(peripheral_control_thread, peripheral_panda, getenv("NO_FAN_CONTROL") != nullptr);
    #ifdef QCOM
    threads.emplace_back(pigeon_thread, peripheral_panda);
    #endif

    threads.emplace_back(can_send_thread, pandas, getenv("FAKESEND") != nullptr);
    threads.emplace_back(can_recv_thread, pandas);

    for (auto &t : threads) t.join();
  }

  // we have exited, clean up pandas
  for (Panda *panda : pandas) {
    delete panda;
  }
}
