# Shared by the nested-session tests (test-host-nested.sh, test-plasma-nested.sh). Source it; it defines functions
# only. They expect MURASAKI_NESTED_MARK and MURASAKI_NESTED_TMP (set by the outer half of the test), the private
# XDG_RUNTIME_DIR, and LOG_DIR for the audio logs.

# Kills every process that carries our mark, except this script and its ancestors. kwin_wayland has file
# capabilities, which make its /proc/PID/environ unreadable, so it is also found by its private socket name.
nested_kill_marked() {
    local signal=$1 skip=" $$ $PPID " pid ppid
    [[ -n ${MURASAKI_NESTED_TMP:-} ]] && pkill "-$signal" -f -- "--socket wayland-$(basename "$MURASAKI_NESTED_TMP") " 2>/dev/null
    [[ -n ${MURASAKI_NESTED_TMP:-} ]] && pkill "-$signal" -f -- "--socket hyprhost-$(basename "$MURASAKI_NESTED_TMP") " 2>/dev/null
    ppid=$PPID
    while [[ $ppid -gt 1 ]]; do
        ppid=$(awk '{print $4}' "/proc/$ppid/stat" 2>/dev/null) || break
        skip+="$ppid "
    done
    for pid in $(ls /proc | grep -E '^[0-9]+$'); do
        [[ $skip == *" $pid "* ]] && continue
        if { tr '\0' '\n' <"/proc/$pid/environ" | grep -qxF "MURASAKI_NESTED_MARK=$MURASAKI_NESTED_MARK"; } 2>/dev/null; then
            kill "-$signal" "$pid" 2>/dev/null
        fi
    done
}


# Starts a private PipeWire + PulseAudio socket with one null sink (see the comment inside) and exports PULSE_SERVER.
nested_start_audio() {
# Audio. A scene engine can hang at start when PulseAudio is unreachable and never
# draw a frame, so the host would show the preview placeholder and a naive test would pass. It gets a private
# PipeWire with a PulseAudio socket and one null sink, and nothing else: a hand-written config instead of the
# system's, a system bus address that leads nowhere, and a session manager only for its linking policy (without
# one, streams are never connected to the sink and their players stall). The stock wireplumber profile would probe
# BlueZ, ALSA and V4L2, i.e. real hardware; the "policy" profile loads none of the device monitors.
PW_CONF=$MURASAKI_NESTED_TMP/pw
mkdir -p "$PW_CONF"
cat >"$PW_CONF/pipewire.conf" <<'EOF'
context.properties = {
    core.daemon = true
    core.name   = pipewire-0
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-access } # without it clients are never granted anything and just hang
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-spa-node-factory }
]
context.objects = [
    # Something has to drive the graph: there is no sound card to do it.
    { factory = spa-node-factory
        args = {
            factory.name    = support.node.driver
            node.name       = Dummy-Driver
            node.group      = pipewire.dummy
            priority.driver = 20000
        }
    }
    { factory = adapter
        args = {
            factory.name            = support.null-audio-sink
            node.name               = murasaki-null-sink
            node.description        = "Murasaki test sink"
            media.class             = Audio/Sink
            audio.position          = [ FL FR ]
            monitor.channel-volumes = true
            object.linger           = true
        }
    }
]
EOF
cat >"$PW_CONF/pipewire-pulse.conf" <<'EOF'
context.properties = {}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-access } # without it clients are never granted anything and just hang
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-protocol-pulse
        args = { server.address = [ "unix:native" ] }
    }
]
stream.properties = {}
pulse.properties = {}
EOF
audio_env=(env DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent PIPEWIRE_CONFIG_DIR="$PW_CONF")
if command -v pipewire >/dev/null && command -v pipewire-pulse >/dev/null; then
    "${audio_env[@]}" pipewire >"$LOG_DIR/pipewire.log" 2>&1 &
    for _ in $(seq 50); do
        [[ -S $XDG_RUNTIME_DIR/pipewire-0 ]] && break
        sleep 0.1
    done
    if command -v wireplumber >/dev/null; then
        "${audio_env[@]}" wireplumber -p policy >"$LOG_DIR/wireplumber.log" 2>&1 &
        sleep 0.5
    fi
    "${audio_env[@]}" pipewire-pulse >"$LOG_DIR/pipewire-pulse.log" 2>&1 &
    for _ in $(seq 50); do
        [[ -S $XDG_RUNTIME_DIR/pulse/native ]] && break
        sleep 0.1
    done
fi
if [[ -S $XDG_RUNTIME_DIR/pulse/native ]]; then
    export PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
    echo "audio: private pipewire-pulse with a null sink"
    if command -v pactl >/dev/null; then
        sleep 1
        { timeout 5 pactl info; timeout 5 pactl list short sinks; timeout 5 pactl list short sources; } \
            >"$LOG_DIR/audio-graph.txt" 2>&1
    fi
else
    echo "audio: no private PulseAudio (scene items may hang; see docs/design/notes)"
fi
}

# Fails (status 1) when the private audio daemons looked at the real machine's devices.
nested_audio_touched_devices() {
    grep -qiE 'bluez|alsa|v4l2|libcamera' "$LOG_DIR/pipewire.log" "$LOG_DIR/wireplumber.log" "$LOG_DIR/pipewire-pulse.log" 2>/dev/null
}

# Writes an ISOLATED session bus config: DIR/bus.conf, which can start only the services whose .service files are
# given (they are copied to DIR/services). A normal session bus also searches the compiled-in
# /usr/share/dbus-1/services, whatever XDG_DATA_DIRS says, and would start the real xdg-desktop-portal, kglobalacceld
# and so on for a call to a name nobody owns. Use it as `dbus-run-session --config-file=DIR/bus.conf -- ...`.
nested_write_bus_conf() {
    local dir=$1 file
    shift
    mkdir -p "$dir/services"
    for file in "$@"; do
        cp "$file" "$dir/services/"
    done
    cat >"$dir/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <servicedir>$dir/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
}
