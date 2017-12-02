from threading import Thread
from time import sleep
import requests
from main import server_host, server_port

def alarm(t):
    data = requests.get('http://{}:{}/current_alarms'.format(server_host, server_port)).text
    if data is not '':
        [print('Alarming ' + d) for d in data.split('\n')]


if __name__ == "__main__":
    while True:
        thread = Thread(target = alarm, args = (1, ))
        thread.start()
        sleep(60)
        thread.join()
