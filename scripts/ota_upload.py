# OTA upload through ESP8266HTTPUpdateServer (POST multipart to /update).
#
# The updater answers HTTP 200 even when flashing fails ("Update error: ..."),
# so the response body is checked instead of the status code.
Import("env")

import urllib.request
import uuid
from os.path import basename


def ota_upload(source, target, env):
    firmware = str(source[0])
    url = env.GetProjectOption("custom_ota_url")

    boundary = uuid.uuid4().hex
    with open(firmware, "rb") as f:
        data = f.read()

    body = (
        "--%s\r\n"
        'Content-Disposition: form-data; name="update"; filename="%s"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n" % (boundary, basename(firmware))
    ).encode() + data + ("\r\n--%s--\r\n" % boundary).encode()

    request = urllib.request.Request(url, data=body, method="POST")
    request.add_header("Content-Type", "multipart/form-data; boundary=%s" % boundary)

    print("Uploading %s (%d bytes) to %s" % (firmware, len(data), url))
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            reply = response.read().decode(errors="replace")
    except Exception as e:
        print("OTA upload failed: %s" % e)
        return 1

    if "Update Success" not in reply:
        print("OTA upload failed, device replied: %s" % reply.strip())
        return 1

    print("OTA update successful, device is rebooting")
    return 0


env.Replace(UPLOADCMD=ota_upload)
