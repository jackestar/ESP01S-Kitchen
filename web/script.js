document.addEventListener("DOMContentLoaded", () => {
  const container = document.getElementById("slider-container");
  const progressCircle = document.getElementById("slider-progress");
  const pwmInput = document.getElementById("pwm_duty_input");
  const pwmValueDisplay = document.getElementById("pwm-value");
  const bd = document.body;
  const bt = document.getElementById("toggle-icon");
  
  const radius = progressCircle.r.baseVal.value;
  const circumference = 2 * Math.PI * radius;
  progressCircle.style.strokeDasharray = circumference;

  let isDragging = false;
  let pauseFormUpdates = false;
  let resumeTimeout;

  // Pause background form updates when the user interacts with the form
  const interactionHandler = () => {
      pauseFormUpdates = true;
      clearTimeout(resumeTimeout);
      // Give the user 10 seconds of no-overwrite time to finish typing/clicking
      resumeTimeout = setTimeout(() => { pauseFormUpdates = false; }, 10000);
  };

  // Listen for interaction on the config form (clicks, typing, focusing)
  document.querySelectorAll('.config-form input, .config-form button').forEach(el => {
      el.addEventListener('focus', interactionHandler);
      el.addEventListener('input', interactionHandler);
      el.addEventListener('click', interactionHandler);
  });

  const updateSlider = (value) => {
    const val = Math.max(0, Math.min(100, value));
    const offset = circumference - (val / 100) * circumference;
    progressCircle.style.strokeDashoffset = offset;
    pwmInput.value = Math.round(val);
    pwmValueDisplay.textContent = `${Math.round(val)}%`;
  };

  const updateColors = (status) => {
    if (status === "Off") {
      bd.style.backgroundColor = "#f0f0f0"; bt.style.fill = "#a5f5a5";
    } else if (status === "On - Temporizador") {
      bd.style.backgroundColor = "#fff4e5"; bt.style.fill = "#f5a5a5";
    } else if (status === "On - Regulado") {
      bd.style.backgroundColor = "#e5f7ff"; bt.style.fill = "#f5a5a5";
    } else if (status === "On - Temporizador & Regulado") {
      bd.style.backgroundColor = "#e5ffe5"; bt.style.fill = "#f5a5a5";
    } else {
      bd.style.backgroundColor = "#e5e5e5"; bt.style.fill = "#a5f5a5";
    }
  };

  const updateState = async () => {
    try {
      const response = await fetch("/state");
      if (!response.ok) return;
      const state = await response.json();

      document.getElementById("status-text").textContent = state.status;
      document.getElementById("output-text").textContent = state.output_state;
      document.getElementById("time-left-text").textContent = state.time_left;

      // Update Slider (unless currently dragging)
      if(!isDragging) {
         updateSlider(state.pwm_duty);
      }
      
      // Update text fields (unless user is currently typing/interacting)
      if (!pauseFormUpdates) {
         document.getElementById("timer_minutes_input").value = state.timer_minutes;
         document.getElementById("initial_on_input").value = state.initial_on_time;
         document.getElementById("pwm_period_input").value = state.pwm_period;
      }

      updateColors(state.status);
    } catch (error) { console.error("Error fetching state:", error); }
  };

  setInterval(updateState, 1000);
  updateState(); // Fetch immediately on load
  
  // --- Slider Logic ---
  const getAngle = (event) => {
    const rect = container.getBoundingClientRect();
    const clientX = event.clientX || event.touches[0].clientX;
    const clientY = event.clientY || event.touches[0].clientY;
    const x = clientX - rect.left - rect.width / 2;
    const y = clientY - rect.top - rect.height / 2;
    let angle = Math.atan2(y, x) * (180 / Math.PI) + 90;
    if (angle < 0) angle += 360;
    return angle;
  };

  const handleSliderInteraction = (event) => {
    event.preventDefault();
    const angle = getAngle(event);
    const value = (angle / 360) * 100;
    updateSlider(value);
  };

  // Prevent slider from triggering when pressing the center power button
  const toggleButton = document.getElementById("toggle-btn");
  toggleButton.addEventListener("mousedown", (e) => e.stopPropagation());
  toggleButton.addEventListener("touchstart", (e) => e.stopPropagation());

  container.addEventListener("mousedown", (e) => { isDragging = true; handleSliderInteraction(e); });
  container.addEventListener("touchstart", (e) => { isDragging = true; handleSliderInteraction(e); });
  document.addEventListener("mousemove", (e) => { if (isDragging) handleSliderInteraction(e); });
  document.addEventListener("touchmove", (e) => { if (isDragging) handleSliderInteraction(e); });

  const endDrag = () => {
    if (isDragging) {
      isDragging = false;
      document.getElementById("control-form").submit();
    }
  };
  
  document.addEventListener("mouseup", endDrag);
  document.addEventListener("touchend", endDrag);

  // --- Timer +/-10 buttons ---
  const incBtn = document.getElementById("timer-increase");
  const decBtn = document.getElementById("timer-decrease");
  const timerInput = document.getElementById("timer_minutes_input");

  const clamp = (v) => Math.max(0, Math.round(v || 0));

  if (incBtn && timerInput) {
    incBtn.addEventListener("click", (e) => {
      e.preventDefault();
      timerInput.value = clamp(parseInt(timerInput.value, 10)) + 10;
    });
  }

  if (decBtn && timerInput) {
    decBtn.addEventListener("click", (e) => {
      e.preventDefault();
      timerInput.value = Math.max(0, clamp(parseInt(timerInput.value, 10)) - 10);
    });
  }
});

// --- Service Worker, Notifications & WebSocket ---
// Attempt WebSocket for realtime events (falls back to existing polling)
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.ready.catch(()=>{});
}

if ('Notification' in window && Notification.permission === 'default') {
  Notification.requestPermission().catch(()=>{});
}

(function tryWebSocket() {
  try {
    const ws = new WebSocket('ws://' + location.hostname + ':81/ws');
    ws.addEventListener('open', () => console.log('WebSocket connected'));
    ws.addEventListener('message', ev => {
      try {
        const payload = JSON.parse(ev.data);
        if (payload.event === 'timer_done') {
          // refresh UI state and notify the user
          try { updateState(); } catch(e){}
          if ('Notification' in window && Notification.permission === 'granted') {
            try {
              new Notification('ESP01S Timer', { body: payload.message || 'Timer finished', icon: '/icons/icon-192.png' });
            } catch (e) {}
          }
        }
      } catch (e) { }
    });
    ws.addEventListener('close', () => console.log('WebSocket closed'));
    ws.addEventListener('error', () => console.log('WebSocket error'));
  } catch (e) {
    console.log('WebSocket init failed', e);
  }
})();
