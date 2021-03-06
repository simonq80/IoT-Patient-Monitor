import requests
import grequests #todo multithreaded rpc request

class thingsboard:
    def __init__(self, host, usr, pwd):
        self.host = 'http://' + host
        self.usr = usr
        self.pwd = pwd
        self.get_auth_token()

    def get_auth_token(self):
        payload = {"username": self.usr, "password": self.pwd}
        resp = requests.post(self.host + '/api/auth/login', json=payload)
        resp.raise_for_status()
        self.auth_header = {'X-Authorization': 'Bearer ' + resp.json()['token']}

    def get_request(self, path, parameters=None):
        resp = requests.get(self.host + path, headers=self.auth_header, params=parameters)
        return resp.text

    def post_request(self, path, body, parameters=None):
        resp = requests.post(self.host + path, headers=self.auth_header, params=parameters, json=body)
        return resp.text + str(resp.status_code)

    @staticmethod
    def response_status_code(resp):
        if resp is None:
            return 408
        return resp.status_code

    def multithread_post_request(self, paths, bodys, parameters=None):
        pb = zip(paths, bodys)
        reqs = (grequests.post(self.host + p, headers=self.auth_header, params=parameters, json=b, timeout=5) for (p, b) in pb)
        resps = grequests.map(reqs)
        return [self.response_status_code(resp) for resp in resps]

    def delete_request(self, path, parameters=None):
        resp = requests.delete(self.host + path, headers=self.auth_header, params=parameters)
        return resp.text

    def actuate_lights(self, device_id, lightNo, state):
        data = {
        "method" : "putLights",
        "params" : {
            "ledno" : lightNo,
            "value" : state
        }}
        return self.post_request("/api/plugins/rpc/oneway/" + device_id, data)

    def multithread_actuate_lights(self, device_ids, lightNos, states):
        dls = list(zip(device_ids, lightNos, states))
        paths = ['/api/plugins/rpc/oneway/' + x[0] for x in dls]
        bodys = [
            {
            "method" : "putLights",
            "params" : {
                "ledno" : x[1],
                "value" : x[2]
            }} for x in dls]
        return self.multithread_post_request(paths, bodys)

    def multithread_set_timer(self, device_ids, seconds):
        paths = ['/api/plugins/rpc/oneway/' + x for x in device_ids]
        bodys = [
            {
            "method" : "putTimer",
            "params" : {
              "seconds" : s
            }} for s in seconds]
        return self.multithread_post_request(paths, bodys)






if __name__ == "__main__":

    a = thingsboard('localhost:8080', 'simonq80@gmail.com', 'admin')

    print(a.auth_header)
    print(a.get_request('/api/admin/updates'))

    print(a.get_request('/api/customer/869c5ee0-d052-11e7-b095-e10f78cbdea4/devices', {'limit': '10'}))

    print(a.post_request('/api/devices', {'deviceTypes': ['node']}))
