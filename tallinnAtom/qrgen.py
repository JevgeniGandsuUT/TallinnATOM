import qrcode
from qrcode.constants import ERROR_CORRECT_L
import matplotlib.pyplot as plt

wifi_ssid = "TallinnAtom"
wifi_password = "12345678"
wifi_type = "WPA"

wifi_string = f"WIFI:T:{wifi_type};S:{wifi_ssid};P:{wifi_password};;"

qr = qrcode.QRCode(error_correction=ERROR_CORRECT_L)
qr.add_data(wifi_string)
qr.make(fit=True)
img = qr.make_image(fill_color="black", back_color="white")

plt.imshow(img, cmap='gray')
plt.axis('off')
plt.show()