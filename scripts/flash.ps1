param(
    [string]$ComPort = "COM3",
    [string]$Environment = "sonoff_s31"
)

pio run -e $Environment -t upload --upload-port $ComPort
