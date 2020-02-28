FROM archlinux
LABEL Description="mqtt build" Vendor="Alex" Version="1.0"
RUN pacman --noconfirm -Syu &&  pacman --noconfirm -S gcc git make mosquitto && git clone https://github.com/alex43dm/mqtt.git && cd mqtt && make
