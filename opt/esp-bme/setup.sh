ln -s /opt/esp-bme/collector.service /etc/systemd/system/esp-bme.service
systemctl start esp-bme
systemctl status esp-bme
systemctl enable esp-bme
