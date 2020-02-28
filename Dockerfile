FROM ubuntu
LABEL Description="mqtt build" Vendor="Alex" Version="1.0"
RUN apt-get update && apt-get -y install build-essential mosquitto mosquitto-dev libmosquitto-dev libmosquitto1 git
RUN git clone https://github.com/alex43dm/mqtt.git
RUN cd mqtt && make
