import requests

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


if __name__ == "__main__":

    a = thingsboard('localhost:8080', 'simonq80@gmail.com', 'admin')

    print(a.auth_header)
    print(a.get_request('/api/admin/updates'))

    print(a.get_request('/api/customer/869c5ee0-d052-11e7-b095-e10f78cbdea4/devices', {'limit': '10'}))

    print(a.post_request('/api/devices', {'deviceTypes': ['node']}))
