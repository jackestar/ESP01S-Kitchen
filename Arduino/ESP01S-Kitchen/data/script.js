2document.addEventListener("DOMContentLoaded", () => {
    const actualStatus = document
        .querySelector(".actual")
        .textContent.trim();
    const bd = document.body;
    const bt = document.querySelector("button[name='toggle'] > svg");
    if (actualStatus === "Off") {
        bd.style.backgroundColor = "#f0f0f0";
        bt.style.fill = "#a5f5a5";
    } else if (actualStatus === "On - Temporizador") {
        bd.style.backgroundColor = "#fff4e5";
        bt.style.fill = "#f5a5a5";
    } else if (actualStatus === "On - Regulado") {
        bd.style.backgroundColor = "#e5f7ff";
        bt.style.fill = "#f5a5a5";
    } else if (actualStatus === "On - Temporizador & Regulado") {
        bd.style.backgroundColor = "#e5ffe5";
        bt.style.fill = "#f5a5a5";
    } else {
        bd.style.backgroundColor = "#e5e5e5";
        bt.style.fill = "#a5f5a5";
    }
    const container = document.getElementById("slider-container");
    const progressCircle = document.getElementById("slider-progress");
    const pwmInput = document.getElementById("pwm_duty_input");
    const pwmValueDisplay = document.getElementById("pwm-value");
    const toggleButton = document.querySelector("button[name=toggle]");

    const radius = progressCircle.r.baseVal.value;
    const circumference = 2 * Math.PI * radius;
    progressCircle.style.strokeDasharray = circumference;

    let isDragging = false;

    updateSlider = (value) => {
        const val = Math.max(0, Math.min(100, value));
        const offset = circumference - (val / 100) * circumference;
        progressCircle.style.strokeDashoffset = offset;
        pwmInput.value = Math.round(val);
        pwmValueDisplay.textContent = `${Math.round(val)}%`;
    };

    getAngle = (event) => {
        const rect = container.getBoundingClientRect();
        const clientX = event.clientX || event.touches[0].clientX;
        const clientY = event.clientY || event.touches[0].clientY;
        const x = clientX - rect.left - rect.width / 2;
        const y = clientY - rect.top - rect.height / 2;
        let angle = Math.atan2(y, x) * (180 / Math.PI) + 90;
        if (angle < 0) {
            angle += 360;
        }
        return angle;
    };

    handleInteraction = (event) => {
        event.preventDefault();
        const angle = getAngle(event);
        const value = (angle / 360) * 100;
        updateSlider(value);
    };

    // Prevent slider interaction when clicking the button
    toggleButton.addEventListener("mousedown", (e) => e.stopPropagation());
    toggleButton.addEventListener("touchstart", (e) => e.stopPropagation());
    // Prevent reload
    // toggleButton.addEventListener('click', (e) => e.preventDefault());

    container.addEventListener("mousedown", (e) => {
        isDragging = true;
        handleInteraction(e);
    });

    container.addEventListener("touchstart", (e) => {
        isDragging = true;
        handleInteraction(e);
    });

    document.addEventListener("mousemove", (e) => {
        if (isDragging) {
            handleInteraction(e);
        }
    });

    document.addEventListener("touchmove", (e) => {
        if (isDragging) {
            handleInteraction(e);
        }
    });

    document.addEventListener("mouseup", (e) => {
        if (isDragging) {
            isDragging = false;
            // Automatically submit the form when the user releases the slider
            document.getElementById("control-form").submit();
        }
    });

    document.addEventListener("touchend", (e) => {
        if (isDragging) {
            isDragging = false;
            document.getElementById("control-form").submit();
        }
    });
    // Set initial value from the server-side variable
    const initialValue = parseInt(pwmInput.value, 10) || 0;
    updateSlider(initialValue);
    const updateState = async () => {
        try {
            const response = await fetch("/state");
            if (!response.ok) return;
            const state = await response.json();

            document.getElementById("status-text").textContent = state.status;
            document.getElementById("output-text").textContent = state.output_state;
            document.getElementById("time-left-text").textContent = state.time_left;

            if (!isDragging) {
                updateSlider(state.pwm_duty);
                document.getElementById("timer_minutes_input").value = state.timer_minutes;
                document.getElementById("initial_on_input").value = state.initial_on_time;
                document.getElementById("pwm_period_input").value = state.pwm_period;
            }
            updateColors(state.status);
        } catch (error) { console.error("Error fetching state:", error); }
    };

    // Fetch state every 1000 milliseconds
    setInterval(updateState, 1000);

    // Initial fetch
    updateState();
});

// Timer +/-10 buttons
(function () {
    const incBtn = document.getElementById("timer-increase");
    const decBtn = document.getElementById("timer-decrease");
    const timerInput = document.getElementById("timer_minutes_input");

    const clamp = (v) => {
        if (isNaN(v) || v === null) return 0;
        return Math.max(0, Math.round(v));
    };

    if (incBtn && timerInput) {
        incBtn.addEventListener("click", (e) => {
            e.preventDefault();
            const cur = clamp(parseInt(timerInput.value, 10) || 0);
            timerInput.value = cur + 10;

            timerInput.placeholder = timerInput.value;
            timerInput.title = timerInput.value;
        });
    }

    if (decBtn && timerInput) {
        decBtn.addEventListener("click", (e) => {
            e.preventDefault();
            const cur = clamp(parseInt(timerInput.value, 10) || 0);
            timerInput.value = Math.max(0, cur - 10);
            timerInput.placeholder = timerInput.value;
            timerInput.title = timerInput.value;
        });
    }
})();