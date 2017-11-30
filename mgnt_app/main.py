from flask import Flask, render_template, request

from flask_sqlalchemy import SQLAlchemy

app = Flask(__name__)
app.config['SQLALCHEMY_DATABASE_URI'] = 'mysql+pymysql://root:mysql@127.0.0.1:32770/db1'
db = SQLAlchemy(app)

class device(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(80))
    address = db.Column(db.String(120), unique=True, nullable=False)

    def __repr__(self):
        return '<User %r>' % self.username

class schedule(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    name = db.Column(db.String(80))

    device_id = db.Column(db.Integer, db.ForeignKey('device.id'),
        nullable=False)
    device = db.relationship('device', backref=db.backref('schedule', lazy=True))

class time(db.Model):
    id = db.Column(db.Integer, primary_key=True)

    hour = db.Column(db.Integer)
    minute = db.Column(db.Integer)

    schedule_id = db.Column(db.Integer, db.ForeignKey('schedule.id'),
        nullable=False)
    schedule = db.relationship('schedule', backref=db.backref('times', lazy=True))



db.create_all()



@app.route('/')
def a():
    return 'Home'

@app.route('/devices', methods=['POST', 'GET'])
@app.route('/devices/<name>')
def d(name=None):
    if request.method == "POST":
        dev = device(name=request.form["name"], address=request.form["address"])
        db.session.add(dev)
        db.session.commit()
    if name is None:
        devs = db.session.query(device).all()
    else:
        devs = db.session.query(device).filter(device.name == name)

    a = [[d.name, d.address, [s.id for s in d.schedule]] for d in devs]
    return render_template('devices.html', x=a)

@app.route('/schedules', methods=['POST', 'GET'])
@app.route('/schedules/<name>')
def s(name=None):
    if request.method == "POST":
        dev = db.session.query(device).filter(device.name == request.form["dname"]).one_or_none()
        tstr = request.form["times"]
        times = tstr.split()
        times = [st.split(':') for st in times]
        times = [[int(t) for t in ti] for ti in times]

        if dev is not None:
            s = schedule(name = request.form["sname"])
            for ti in times:
                t = time(hour=ti[0], minute=ti[1])
                s.times.append(t)
            dev.schedule.append(s)
            db.session.add(dev)
            db.session.commit()

    if name is None:
        sch = db.session.query(schedule).all()
    else:
        sch = db.session.query(schedule).filter(schedule.id == name)

    a = [[s.id, s.name, s.device.name, str([(str(t.hour) + ':' + str(t.minute)) for t in s.times])] for s in sch]
    return render_template('schedules.html', x=a)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port="8000")
