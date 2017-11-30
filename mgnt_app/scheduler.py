from threading import Thread
from time import sleep
import requests


def alarm(t):
    data = requests.get('http://127.0.0.1:8000/current_alarms').text
    [print('Alarming ' + d) for d in data.split('\n')]


if __name__ == "__main__":
    while True:
        thread = Thread(target = alarm, args = (1, ))
        thread.start()
        sleep(60)
        thread.join()
