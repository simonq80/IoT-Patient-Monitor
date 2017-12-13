from flask import Flask, render_template, request

from flask_sqlalchemy import SQLAlchemy
from datetime import datetime
import configparser
import json
import requests
from thingsboard_api import thingsboard
from threading import Thread
from time import sleep

c = configparser.ConfigParser()
c.read('config.cfg')
c = c['MAIN']
db_path = 'mysql+pymysql://{}:{}@{}:{}/{}'.format(c['MYSQL_USER'],
    c['MYSQL_PASS'], c['MYSQL_HOST'], c['MYSQL_PORT'], c['MYSQL_DB'])
server_host = c['SERVER_HOST']
server_port = c['SERVER_PORT']
dashboard_id = c['DASHBOARD_ID']
tb_host = c['TB_HOST']
tb_port = c['TB_PORT']
tb_user = c['TB_USER']
tb_pass = c['TB_PASS']
tb = thingsboard('{}:{}'.format(tb_host, tb_port), tb_user, tb_pass)


app = Flask(__name__)
app.config['SQLALCHEMY_DATABASE_URI'] = db_path
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
db = SQLAlchemy(app)

class device(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(80), unique=True, nullable=False)
    address = db.Column(db.String(120), unique=True, nullable=False)

    def delete(self):
        for sch in self.schedule:
            sch.delete()
        db.session.commit()
        db.session.delete(self)
        db.session.commit()

class schedule(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(80))

    device_id = db.Column(db.Integer, db.ForeignKey('device.id'),
        nullable=False)
    device = db.relationship('device', backref=db.backref('schedule', lazy=True))

    def delete(self):
        for time in self.times:
            db.session.delete(time)
        db.session.commit()
        db.session.delete(self)
        db.session.commit()

class time1(db.Model):
    id = db.Column(db.Integer, primary_key=True)

    hour = db.Column(db.Integer, nullable=False)
    minute = db.Column(db.Integer, nullable=False)

    schedule_id = db.Column(db.Integer, db.ForeignKey('schedule.id'),
        nullable=False)
    schedule = db.relationship('schedule', backref=db.backref('times', lazy=True))

@app.route('/metrics')
def metrics():
    return render_template('metrics.html', db_id=dashboard_id)

@app.route('/')
@app.route('/devices', methods=['POST', 'GET'])
@app.route('/devices/<name>')
@app.route('/devices/<name>/<remove>')
def d(name=None, remove=None):
    if request.method == "POST":
        dev = device(name=request.form["name"], address=request.form["address"])
        db.session.add(dev)
        db.session.commit()

    if remove is not None:
        dev = db.session.query(device).filter(device.name == name).one_or_none()
        if dev:
            dev.delete()
        name = None

    if name is None:
        devs = db.session.query(device).all()
    else:
        devs = db.session.query(device).filter(device.name == name)

    a = [[d.name, d.address, [s.id for s in d.schedule]] for d in devs]
    return render_template('devices.html', x=a)

@app.route('/schedules', methods=['POST', 'GET'])
@app.route('/schedules/<name>')
@app.route('/schedules/<name>/<remove>')
def s(name=None, remove=None):
    if request.method == "POST":
        dev = db.session.query(device).filter(device.name == request.form["dname"]).one_or_none()
        tstr = request.form["times"]
        times = tstr.split()
        times = [st.split(':') for st in times]
        times = [[int(t) for t in ti] for ti in times]

        if dev is not None:
            s = schedule(name = request.form["sname"])
            for ti in times:
                t = time1(hour=ti[0], minute=ti[1])
                s.times.append(t)
            dev.schedule.append(s)
            db.session.add(dev)
            db.session.commit()

    if remove is not None:
        sch = db.session.query(schedule).filter(schedule.id == name).one_or_none()
        if sch:
            sch.delete()
        name = None

    if name is None:
        sch = db.session.query(schedule).all()
    else:
        sch = db.session.query(schedule).filter(schedule.id == name)

    a = [[s.id, s.name, s.device.name, str([(str(t.hour) + ':' + str(t.minute)) for t in s.times])] for s in sch]
    return render_template('schedules.html', x=a)

def get_next_alarm_seconds(alarms):
    if len(alarms) == 0:
        return 1000000
    alarms = [3600 * hour + 60 * minute for (hour, minute) in alarms]
    now = datetime.now()
    now = 3600 * now.hour + 60 * now.minute + now.second
    n = 1000000
    for alarm in alarms:
        if alarm - now > 0 and alarm - now < n:
            n = alarm - now
    if n == 1000000:
        n = min(alarms) + (86400 -now)
    return n


@app.route('/next_alarms')
def next_alarms():
    devices = db.session.query(device)
    toReturn = {}
    for dev in devices:
        times = []
        for schedule in dev.schedule:
            for time in schedule.times:
                times.append((time.hour, time.minute))
        toReturn[dev.address] = get_next_alarm_seconds(times)

    return json.dumps(toReturn)

@app.route('/add_devices')
def add_devices():
    customer_data = json.loads(tb.get_request('/api/customers', {'limit': 100}))
    customer_ids = [d['id']['id'] for d in customer_data['data']]
    dev_ids = []
    for cid in customer_ids:
        dev_data = json.loads(tb.get_request('/api/customer/{}/devices'.format(cid), {'limit': 100}))
        device_ids = [{'name': d['name'], 'id': d['id']['id']} for d in dev_data['data']]
        for dev in device_ids:
            dev_ids.append(dev)
    for dev in dev_ids:
        if db.session.query(device).filter(device.name == dev['name']).one_or_none() is None and\
            db.session.query(device).filter(device.address == dev['id']).one_or_none() is None:
            dev = device(name=dev['name'], address=dev['id'])
            db.session.add(dev)
            db.session.commit()
    return d()


def device_update(host, port):
    while True:
        sleep(10    )
        data = requests.get('http://{}:{}/next_alarms'.format(host, port)).json()

        for deviceId in data:
            print('ID: {} Seconds: {}'.format(deviceId, data[deviceId]))

        ids = [id for id in data]
        times = [data[id] for id in data]
        print(tb.multithread_set_timer(ids, times))





if __name__ == "__main__":
    db.create_all()
    thread = Thread(target = device_update, args = (server_host, server_port))
    thread.start()
    app.run(host=server_host, port=int(server_port))
