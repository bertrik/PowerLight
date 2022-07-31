# PowerLight
Arduino project showing current power generation mix as a pie chart on a LED ring light

Compiling:
  pio run

Compiling and flashing over serial:
  pio run -t upload

Compiling and flashing over-the-air:
  pio run -t upload --upload-port=<ip>
Find the ip using
  ping esp8266-powerlight

