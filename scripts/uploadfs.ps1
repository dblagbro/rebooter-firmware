param(
    [string]$ComPort = "COM3",
    [string]$Environment = "sonoff_s31"
)

pio run -e $Environment -t uploadfs --upload-port $ComPort
