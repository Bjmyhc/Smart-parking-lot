/* ==========================================================================
 * 智能停车场监控系统 - Web前端逻辑 (重构版)
 *
 * 架构分层:
 *   config    - 常量配置
 *   ui        - DOM 操作统一入口 (所有元素引用 + 离线/在线切换)
 *   connStatus- 在线状态判定 (纯逻辑, 通过 ui.setOnline 通知)
 *   ledSwitch - LED开关状态机 (idle→loading→confirming→idle/error)
 *   occTimer  - 占用时间本地计时 (1s插值 + 僵尸车本地判定)
 *   api       - OneNet HTTP 请求
 *
 * 对接 OneNet 物联网平台 API:
 *   - GET  query-device-property  实时获取设备属性
 *   - POST set-device-property    远程设置设备属性
 *
 * 物模型属性:
 *   ParkStatus    int   0=空闲, 1=有车, 2=疑似僵尸车
 *   GeoMagnetic   int   0=无车, 1=有车
 *   Ultrasonic    int   超声波距离(cm)
 *   OccupiedTime  int   占用时间(s)
 *   LED           bool  LED实际亮灭状态
 *   LedEnable     bool  LED云端使能标志 (可远程设置)
 * ========================================================================== */

const config = {
  authorization: "version=2018-10-31&res=products%2F53BV12EYcY%2Fdevices%2Fpark1&et=1815919855&method=md5&sign=WSMSUPJ7K3%2B%2ByxiohKCfFQ%3D%3D",
  product_id: "53BV12EYcY",
  device_name: "park1",
  getinfo_url: "https://iot-api.heclouds.com/thingmodel/query-device-property?product_id=53BV12EYcY&device_name=park1",
  setinfo_url: "https://iot-api.heclouds.com/thingmodel/set-device-property",
  command_timeout: 20000,       /* 命令超时(ms): OneNet同步API, LoRa链路延迟大 */
  switch_cooldown: 5000,        /* 操作频率限制(ms): 防止快速反复拨动压垮LoRa链路 */
  confirm_window: 30000,        /* 确认窗口(ms): >设备上报周期15s + LoRa延迟余量 */
  online_timeout: 45000,        /* 在线判据: 45s(3倍上报周期)无新上报判离线 */
  offline_fallback: 60000,      /* 备用判据: 60s无成功查询判离线 */
  poll_interval: 3000,          /* 轮询间隔(ms) */
  zombie_threshold: 5,          /* 僵尸车阈值(秒), 与设备端一致 */
};

/* ==========================================================================
 * ui - DOM 操作统一入口
 *
 * 所有 DOM 元素引用和操作集中于此, 其他模块不直接操作 DOM。
 * setOnline(online) 是离线/在线切换的唯一入口, 集中处理:
 *   - 连接状态文字/样式
 *   - 开关禁用/启用
 *   - 数据显示重置为 -- / 恢复
 *   - occTimer 启停
 * ========================================================================== */
const ui = {
  el: {},   /* DOM 元素缓存 */

  init() {
    const ids = [
      'conn-status', 'led-enable-switch', 'led-enable-switch-label',
      'led-switch-loading', 'led-operation-feedback',
      'park-status', 'geo-magnetic', 'ultrasonic',
      'occupied-time', 'led-state',
    ];
    ids.forEach(id => {
      this.el[id] = document.getElementById(id);
    });
  },

  /* 离线/在线切换的唯一入口: 集中处理所有 UI 变化 */
  setOnline(online) {
    const e = this.el;
    /* 连接状态 */
    e['conn-status'].textContent = online ? '设备在线' : '设备离线';
    e['conn-status'].className = 'conn-status ' + (online ? 'conn-online' : 'conn-offline');

    /* 开关: loading 时保持禁用, 否则按在线状态决定 */
    if (online && !ledSwitch.isLoading()) {
      e['led-enable-switch'].disabled = false;
    } else {
      e['led-enable-switch'].disabled = true;
    }

    if (online) {
      /* 在线: 恢复 occTimer */
      occTimer.active = true;
    } else {
      /* 离线: 停止 occTimer + 所有数据重置为 -- */
      occTimer.active = false;
      e['park-status'].textContent = '--';
      e['park-status'].className = 'data-value status-value';
      e['geo-magnetic'].textContent = '--';
      e['geo-magnetic'].className = 'data-value mag-value';
      e['ultrasonic'].textContent = '--';
      e['occupied-time'].textContent = '--';
      e['led-state'].textContent = '--';
      e['led-state'].className = 'data-value led-value';
    }
  },

  /* --- 数据更新方法 --- */
  setConnStatus(text, cls) {
    this.el['conn-status'].textContent = text;
    this.el['conn-status'].className = 'conn-status ' + cls;
  },

  setGeoMag(detected) {
    this.el['geo-magnetic'].textContent = detected ? '有车' : '无车';
    this.el['geo-magnetic'].className = 'data-value mag-value ' + (detected ? 'mag-detected' : 'mag-clear');
  },

  setUltrasonic(val) {
    this.el['ultrasonic'].textContent = val;
  },

  setLedState(on) {
    this.el['led-state'].textContent = on ? '点亮' : '熄灭';
    this.el['led-state'].className = 'data-value led-value ' + (on ? 'led-on' : 'led-off');
  },

  /* --- 开关相关 --- */
  setSwitch(checked, disabled) {
    this.el['led-enable-switch'].checked = checked;
    this.el['led-enable-switch'].disabled = disabled;
  },

  setSwitchLoading(loading) {
    const label = this.el['led-enable-switch-label'];
    const spinner = this.el['led-switch-loading'];
    if (loading) {
      label.classList.add('loading');
      spinner.classList.add('active');
    } else {
      label.classList.remove('loading');
      spinner.classList.remove('active');
    }
  },

  setFeedback(text, cls) {
    const el = this.el['led-operation-feedback'];
    el.textContent = text;
    el.className = 'operation-feedback ' + (cls || '');
  },

  clearFeedback() {
    const el = this.el['led-operation-feedback'];
    el.textContent = '';
    el.className = 'operation-feedback';
  },

  /* --- 占用时间 + 车位状态 (由 occTimer 调用) --- */
  setOccTime(val) {
    this.el['occupied-time'].textContent = val;
  },

  setParkStatus(text, cls) {
    this.el['park-status'].textContent = text;
    this.el['park-status'].className = 'data-value status-value ' + (cls || '');
  },
};

/* ==========================================================================
 * connStatus - 设备在线状态判定 (纯逻辑, 不直接操作 DOM)
 *
 * 原理: OneNet query API 返回设备最近上报的属性快照(含 time 时间戳)。
 *   设备在线时每15s上报, time 持续刷新; 离线时 time 是旧值, 随时间过期。
 *   以"属性上报时间新鲜度"为在线判据:
 *     - 45s(3倍上报周期)内有新上报 → 在线
 *     - 超过45s无新上报 → 离线
 *   无 time 时退化为"60s内有成功查询即在线"。
 * ========================================================================== */
const connStatus = {
  lastReportTime: 0,
  lastQueryTime: 0,
  wasOnline: null,

  markReport(reportTime) {
    if (reportTime > 0) this.lastReportTime = reportTime;
    this.lastQueryTime = Date.now();
    this.checkChange();
  },

  markSuccess() {
    this.lastQueryTime = Date.now();
    this.checkChange();
  },

  markError() { /* 不立即判离线, 由 tick 统一裁决 */ },

  isOnline() {
    const now = Date.now();
    if (this.lastReportTime > 0) {
      return (now - this.lastReportTime < config.online_timeout);
    }
    return (now - this.lastQueryTime < config.offline_fallback);
  },

  /* 每秒 tick: 检测在线状态变化, 通知 ui */
  tick() {
    this.checkChange();
  },

  /* 检测状态变化并通知 ui; 离线→在线时触发设备重启处理 */
  checkChange() {
    const online = this.isOnline();
    if (this.wasOnline === false && online) {
      handleDeviceRestart();
    }
    this.wasOnline = online;
    ui.setOnline(online);
  },
};

/* ==========================================================================
 * ledSwitch - LED 使能开关状态机
 *
 * 状态: idle → loading → confirming → idle
 *                  \-> error → idle
 *
 * 命令发出时进入 loading(乐观更新+锁定);
 *   code=200 → confirming(成功, 等设备上报确认);
 *   错误码   → error(回弹开关, 提示失败);
 *   超时     → error(回弹开关, 提示超时);
 * confirming 窗口内设备上报 == 目标状态 → 确认完成;
 *   窗口超时: settled 以目标为准, 未 settled 以云端真实状态收敛。
 * ========================================================================== */
const ledSwitch = {
  state: 'idle',
  pendingCommand: null,
  lastKnownState: false,
  commandCounter: 0,
  confirmDeadline: 0,
  feedbackTimer: null,
  lastActionTime: 0,

  isLoading() {
    return this.state === 'loading';
  },

  generateCommandId() {
    this.commandCounter++;
    return `cmd_${Date.now()}_${this.commandCounter}`;
  },

  clearFeedbackTimer() {
    if (this.feedbackTimer) {
      clearTimeout(this.feedbackTimer);
      this.feedbackTimer = null;
    }
  },

  /* 进入 loading: 乐观更新 + 锁定 */
  enterLoading(targetState) {
    this.clearFeedbackTimer();
    this.confirmDeadline = 0;

    const commandId = this.generateCommandId();
    this.state = 'loading';
    this.pendingCommand = {
      commandId,
      targetState,
      timestamp: Date.now(),
      settled: false,
    };

    ui.setSwitch(targetState, true);
    ui.setSwitchLoading(true);
    ui.setFeedback('正在切换...', 'feedback-loading');
    return commandId;
  },

  /* 命令成功: 进入确认窗口 */
  completeSuccess(commandId) {
    if (!this.matchCommand(commandId)) return;
    this.pendingCommand.settled = true;
    this.confirmDeadline = this.pendingCommand.timestamp + config.confirm_window;
    this.state = 'confirming';

    ui.setSwitch(this.pendingCommand.targetState, false);
    ui.setSwitchLoading(false);
    ui.setFeedback('切换成功', 'feedback-success');
    this.scheduleFeedbackClear(1500);
  },

  /* 命令失败(错误码/网络异常): 回弹开关 */
  completeError(commandId, errorMsg) {
    if (!this.matchCommand(commandId)) return;
    this.resetToIdle();
    ui.setSwitch(this.lastKnownState, false);
    ui.setSwitchLoading(false);
    ui.setFeedback(errorMsg || '切换失败，请重试', 'feedback-error');
    this.scheduleFeedbackClear(3000);
  },

  /* 超时: 回弹开关 */
  handleTimeout(commandId) {
    if (!this.matchCommand(commandId)) return;
    this.resetToIdle();
    ui.setSwitch(this.lastKnownState, false);
    ui.setSwitchLoading(false);
    ui.setFeedback('命令超时，请重试', 'feedback-error');
    this.scheduleFeedbackClear(3000);
  },

  /* 处理轮询上报的 LedEnable: 状态机调和入口 */
  handleCloudReport(cloudState) {
    if (this.state === 'loading') return;

    if (this.state === 'confirming') {
      if (Date.now() > this.confirmDeadline) {
        /* 窗口超时: settled 以目标为准, 否则以云端真实状态收敛 */
        this.confirmDone(this.pendingCommand.settled
          ? this.pendingCommand.targetState
          : cloudState);
      } else if (cloudState === this.pendingCommand.targetState) {
        this.confirmDone(cloudState);
      }
      return;
    }

    /* idle: 直接同步 */
    this.syncFromCloud(cloudState);
  },

  /* --- 内部方法 --- */

  matchCommand(commandId) {
    if (!this.pendingCommand || this.pendingCommand.commandId !== commandId) {
      console.warn('命令ID不匹配，忽略');
      return false;
    }
    return true;
  },

  resetToIdle() {
    this.state = 'idle';
    this.pendingCommand = null;
    this.confirmDeadline = 0;
  },

  confirmDone(state) {
    this.lastKnownState = state;
    this.resetToIdle();
    ui.setSwitch(state, false);
  },

  syncFromCloud(cloudState) {
    this.lastKnownState = cloudState;
    ui.setSwitch(cloudState, false);
  },

  scheduleFeedbackClear(ms) {
    this.clearFeedbackTimer();
    this.feedbackTimer = setTimeout(() => {
      ui.clearFeedback();
      this.feedbackTimer = null;
    }, ms);
  },
};

/* ==========================================================================
 * occTimer - 占用时间本地计时 + 车位状态联动
 *
 * 解决设备15s上报间隔内的显示空白:
 *   - 云端基准: ParkStatus(0/1/2) 决定占用启停
 *   - 本地插值: 1s ticker 平滑累加 OccupiedTime
 *   - 本地判定: 占用>=5s 立即显示僵尸车, 不等云端上报
 * ========================================================================== */
const STATUS_MAP = {0: "空闲", 1: "有车", 2: "僵尸车"};
const STATUS_COLOR_MAP = {0: "status-idle", 1: "status-occupied", 2: "status-zombie"};

function computeParkDisplay(lastCloudStatus, localOccupied) {
  if (lastCloudStatus === 0) return 0;
  if (localOccupied >= config.zombie_threshold) return 2;
  if (lastCloudStatus === 2) return 2;
  if (lastCloudStatus === 1) return 1;
  return lastCloudStatus;
}

const occTimer = {
  state: 'idle',
  active: true,             /* 由 ui.setOnline 控制 */
  anchorSeconds: 0,
  anchorTimestamp: 0,
  lastCloudStatus: null,
  lastCloudOcc: 0,
  tickIntervalId: null,

  currentOccupied() {
    if (this.state === 'running') {
      return this.anchorSeconds + Math.floor((Date.now() - this.anchorTimestamp) / 1000);
    }
    return this.anchorSeconds;
  },

  refreshDisplay() {
    if (!this.active) return;
    const occ = this.currentOccupied();
    ui.setOccTime(occ);
    if (this.lastCloudStatus === null) return;
    const disp = computeParkDisplay(this.lastCloudStatus, occ);
    ui.setParkStatus(STATUS_MAP[disp] || "未知", STATUS_COLOR_MAP[disp] || "");
  },

  init() {
    if (this.tickIntervalId === null) {
      this.tickIntervalId = setInterval(() => this.tick(), 1000);
    }
  },

  tick() {
    this.refreshDisplay();
  },

  calibrate(occupiedSeconds) {
    if (this.lastCloudStatus === 2) return;  /* 僵尸车锁定: 本地驱动 */
    const localEstimate = this.anchorSeconds + Math.floor((Date.now() - this.anchorTimestamp) / 1000);
    this.anchorSeconds = Math.max(occupiedSeconds, localEstimate);
    this.anchorTimestamp = Date.now();
    this.lastCloudOcc = occupiedSeconds;
    this.refreshDisplay();
  },

  start(initialSeconds) {
    if (this.state === 'running') {
      if (this.lastCloudStatus !== 2) {
        this.calibrate(initialSeconds);
      } else {
        this.lastCloudOcc = initialSeconds;
      }
      return;
    }
    this.state = 'running';
    this.anchorSeconds = initialSeconds;
    this.anchorTimestamp = Date.now();
    this.lastCloudOcc = initialSeconds;
    this.refreshDisplay();
  },

  reset() {
    this.state = 'idle';
    this.anchorSeconds = 0;
    this.anchorTimestamp = 0;
    this.lastCloudOcc = 0;
    this.lastCloudStatus = 0;
    this.refreshDisplay();
  },

  updateCloudParkStatus(statusNumber, occupiedWhenOccupied) {
    if (statusNumber === undefined || statusNumber === null || isNaN(statusNumber)) return;

    if (statusNumber === 0) {
      this.reset();
      return;
    }
    if (statusNumber === 2) {
      this.lastCloudStatus = 2;
      if (this.state !== 'running') {
        this.start(Number(occupiedWhenOccupied) || 0);
      } else {
        this.lastCloudOcc = Number(occupiedWhenOccupied) || this.lastCloudOcc;
        this.refreshDisplay();
      }
      return;
    }
    /* ParkStatus === 1 */
    this.lastCloudStatus = 1;
    this.start(Number(occupiedWhenOccupied) || 0);
  },
};

/* ==========================================================================
 * 辅助函数
 * ========================================================================== */
function extractValue(v) {
  if (v !== null && typeof v === 'object' && 'value' in v) return v.value;
  return v;
}

function parseReportTime(t) {
  if (t === undefined || t === null) return NaN;
  let n = (typeof t === 'number') ? t : Number(t);
  if (!isNaN(n)) return (n < 1e11) ? n * 1000 : n;
  const d = Date.parse(String(t).replace(' ', 'T'));
  return isNaN(d) ? NaN : d;
}

function isTrue(v) {
  return (v === true || v === 1 || v === "true" || v === "1");
}

/* ==========================================================================
 * 设备重启处理
 * ========================================================================== */
function handleDeviceRestart() {
  console.log("[设备重启] 检测到设备重新上线, 重置本地状态");
  occTimer.reset();
}

/* ==========================================================================
 * 开关事件处理
 * ========================================================================== */
function handleSwitchChange(switchEl) {
  /* 离线禁止操作 */
  if (!connStatus.isOnline()) {
    switchEl.checked = ledSwitch.lastKnownState;
    ledSwitch.clearFeedbackTimer();
    ui.setFeedback('设备离线，无法操作', 'feedback-error');
    ledSwitch.scheduleFeedbackClear(2000);
    return;
  }

  /* loading 中禁止操作 */
  if (ledSwitch.isLoading()) {
    switchEl.checked = ledSwitch.lastKnownState;
    return;
  }

  /* 操作频率限制 */
  const now = Date.now();
  if (ledSwitch.lastActionTime && (now - ledSwitch.lastActionTime) < config.switch_cooldown) {
    const remain = Math.ceil((config.switch_cooldown - (now - ledSwitch.lastActionTime)) / 1000);
    switchEl.checked = ledSwitch.lastKnownState;
    ui.setFeedback(`操作过于频繁，请${remain}秒后再试`, 'feedback-error');
    ledSwitch.scheduleFeedbackClear(2000);
    return;
  }

  const targetState = switchEl.checked;
  ledSwitch.lastActionTime = now;
  const commandId = ledSwitch.enterLoading(targetState);
  sendLedEnableCommand(targetState, commandId);
}

/* ==========================================================================
 * OneNet API 请求
 * ========================================================================== */
function sendLedEnableCommand(targetState, commandId) {
  const timeoutId = setTimeout(() => {
    ledSwitch.handleTimeout(commandId);
  }, config.command_timeout);

  fetch(config.setinfo_url, {
    method: "POST",
    headers: {
      "authorization": config.authorization,
      "Content-Type": "application/json"
    },
    body: JSON.stringify({
      "product_id": config.product_id,
      "device_name": config.device_name,
      "params": { "LedEnable": targetState }
    })
  })
  .then(response => response.json())
  .then(data => {
    clearTimeout(timeoutId);
    console.log("设置LED使能响应:", JSON.stringify(data));
    const code = Number(data.code);
    if (code === 200 || code === 0) {
      ledSwitch.completeSuccess(commandId);
    } else {
      ledSwitch.completeError(commandId, data.msg || '切换失败');
    }
  })
  .catch(error => {
    clearTimeout(timeoutId);
    console.error("设置LED使能失败:", error);
    ledSwitch.completeError(commandId, '网络错误，请重试');
  });
}

function getInfo() {
  fetch(config.getinfo_url, {
    method: "GET",
    headers: { "authorization": config.authorization }
  })
  .then(response => response.json())
  .then(data => {
    const props = {};
    let latestReportTime = 0;
    const container = data && data.data ? data.data : data;
    const list =
      (Array.isArray(container) && container) ||
      (Array.isArray(container.items) && container.items) ||
      (Array.isArray(container.properties) && container.properties) ||
      (Array.isArray(data && data.items) && data.items) ||
      (Array.isArray(data && data.properties) && data.properties);

    if (list) {
      list.forEach(item => {
        if (item && item.identifier !== undefined) {
          props[item.identifier] = extractValue(item.value);
          const t = parseReportTime(item.time);
          if (!isNaN(t)) latestReportTime = Math.max(latestReportTime, t);
        }
      });
    } else if (container && typeof container === 'object') {
      Object.keys(container).forEach(key => {
        if (key !== 'items' && key !== 'properties') {
          props[key] = extractValue(container[key]);
          const t = parseReportTime(container[key]);
          if (!isNaN(t)) latestReportTime = Math.max(latestReportTime, t);
        }
      });
    }

    /* 更新在线状态 (会触发 ui.setOnline) */
    if (latestReportTime > 0) {
      connStatus.markReport(latestReportTime);
    } else {
      connStatus.markSuccess();
    }

    /* 离线时不填充旧快照数据 */
    if (!connStatus.isOnline()) return;

    /* 地磁 */
    if (props.GeoMagnetic !== undefined) {
      ui.setGeoMag(Number(props.GeoMagnetic));
    }
    /* 超声波 */
    if (props.Ultrasonic !== undefined) {
      ui.setUltrasonic(props.Ultrasonic);
    }
    /* 车位状态 + 占用时间 */
    const parkStatusNum = Number(props.ParkStatus);
    const parkStatusValid = props.ParkStatus !== undefined;
    const occNum = Number(props.OccupiedTime);
    const occValid = props.OccupiedTime !== undefined;
    if (parkStatusValid) {
      occTimer.updateCloudParkStatus(parkStatusNum, occValid ? occNum : undefined);
    } else if (occValid && occTimer.state === 'running') {
      occTimer.calibrate(occNum);
    }
    /* LED 状态 */
    if (props.LED !== undefined) {
      ui.setLedState(isTrue(props.LED));
    }
    /* LED 使能开关 (走状态机调和) */
    if (props.LedEnable !== undefined) {
      ledSwitch.handleCloudReport(isTrue(props.LedEnable));
    }
  })
  .catch(error => {
    connStatus.markError();
    console.error("获取设备属性失败:", error);
  });
}

/* ==========================================================================
 * 兼容旧接口
 * ========================================================================== */
function setLedEnable(isChecked) {
  console.warn('setLedEnable已废弃，请使用handleSwitchChange');
}

/* ==========================================================================
 * 启动
 * ========================================================================== */
window.onload = function() {
  ui.init();
  occTimer.init();
  connStatus.wasOnline = null;
  getInfo();
  setInterval(getInfo, config.poll_interval);
  setInterval(() => connStatus.tick(), 1000);
};
