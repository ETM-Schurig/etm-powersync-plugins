#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sonde le plugin openmeteo sur une box nymea réelle, par l'API JSON-RPC.

POURQUOI CET OUTIL EXISTE
Compiler et passer un test réseau ne dit rien d'un cycle réel. Les défauts trouvés
pendant la mise au point ne pouvaient être vus qu'ici :

  - le plan PV ne pouvait pas être créé (AddThing ne porte pas de parentId) ;
  - deux requêtes sur cinq ne laissaient aucune trace ;
  - la courbe embarquait la clé de révision du run PRÉCÉDENT — visible seulement
    en comparant deux états après deux runs consécutifs ;
  - les plans d'un même site dérivaient les uns des autres.

Aucun n'était détectable à la compilation.

PARTICULARITÉS DE L'API, qui coûtent une demi-heure à qui les redécouvre
  - Le port 2222 est en TLS. Une connexion en clair ne répond RIEN — pas d'erreur,
    juste un silence, ce qui fait croire à un mauvais port.
  - Après JSONRPC.Authenticate il faut REFAIRE JSONRPC.Hello, sinon tout appel
    échoue sur « Changing the user (token) requires a new handshake ».
  - Les notifications exigent un abonnement explicite par espace de noms
    (JSONRPC.SetNotificationStatus), sinon on n'en reçoit aucune.

USAGE
    python3 bench_probe.py --host <adresse-de-la-box> --user <utilisateur> --password '…' [--watch 60]

Le mot de passe peut aussi venir de l'environnement (NYMEA_PASSWORD) pour ne pas
figurer dans l'historique du shell. Aucun identifiant n'est écrit sur disque.
"""

import argparse
import datetime as dt
import itertools
import json
import os
import socket
import ssl
import sys
import time


class Nymea:
    def __init__(self, host, port=2222, timeout=40):
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE  # self-signed certificate of the box
        self.sock = context.wrap_socket(socket.create_connection((host, port), timeout=timeout))
        self.sock.settimeout(timeout)
        self.buffer = b""
        self.ids = itertools.count(1)
        self.token = None
        self.hello = self.call("JSONRPC.Hello")["params"]

    def _line(self):
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connexion fermée")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line

    def call(self, method, params=None):
        request_id = next(self.ids)
        message = {"id": request_id, "method": method}
        if params:
            message["params"] = params
        if self.token:
            message["token"] = self.token
        self.sock.sendall((json.dumps(message) + "\n").encode())
        while True:
            reply = json.loads(self._line().decode())
            if reply.get("id") == request_id and "notification" not in reply:
                if reply.get("status") != "success":
                    raise RuntimeError(f"{method}: {reply.get('error')}")
                return reply

    def authenticate(self, user, password):
        reply = self.call("JSONRPC.Authenticate", {
            "username": user, "password": password, "deviceName": "openmeteo-bench-probe"})
        if not reply["params"].get("success"):
            raise SystemExit("authentification refusée")
        self.token = reply["params"]["token"]
        # Mandatory: the server refuses any call after a token change
        # until a new handshake has taken place.
        self.hello = self.call("JSONRPC.Hello")["params"]


def stamp(value):
    return dt.datetime.fromtimestamp(value).strftime("%d/%m %H:%M") if value else "0 (inconnu)"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="<adresse-de-la-box>")
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", default=os.environ.get("NYMEA_PASSWORD"))
    parser.add_argument("--watch", type=int, default=0,
                        help="secondes d'écoute des notifications StateChanged après le relevé")
    args = parser.parse_args()
    if not args.password:
        raise SystemExit("mot de passe requis (--password ou NYMEA_PASSWORD)")

    nymea = Nymea(args.host)
    nymea.authenticate(args.user, args.password)
    print(f"serveur : {nymea.hello['name']}\n")

    classes = {c["id"]: c for c in nymea.call("Integrations.GetThingClasses")["params"]["thingClasses"]}
    things = nymea.call("Integrations.GetThings")["params"]["things"]

    curve_state_ids = {}
    for thing in things:
        thing_class = classes[thing["thingClassId"]]
        if "openMeteo" not in thing_class["name"]:
            continue
        values = {v["stateTypeId"]: v["value"]
                  for v in nymea.call("Integrations.GetStateValues", {"thingId": thing["id"]})["params"]["values"]}
        states = {s["name"]: values.get(s["id"]) for s in thing_class["stateTypes"]}
        print(f"=== {thing_class['name']} — {thing['name']}")
        for name in sorted(states):
            value = states[name]
            if name.endswith(("Time", "At")) and isinstance(value, int) and value:
                value = stamp(value)
            if name == "forecastCurveTransport" and isinstance(value, dict) and value:
                curve_state_ids[thing["id"]] = next(
                    s["id"] for s in thing_class["stateTypes"] if s["name"] == name)
                points = value.get("p", [])
                value = (f"{len(points)} créneaux · t0={stamp(value.get('t0'))} "
                         f"· issuedAt={stamp(value.get('issuedAt'))} · modèle={value.get('model')} "
                         f"· total {sum(points) / 1000:.1f} kWh")
            print(f"   {name:34} = {value}")
        missing = [k for k, v in states.items() if v is None]
        print(f"   états non renseignés : {missing or 'aucun'}\n")

    if not args.watch:
        return 0

    # Count the actual notifications rather than assume they arrive:
    # an unchanged value is neither stored nor notified (CONTRACT_A sec. 5.4).
    nymea.call("JSONRPC.SetNotificationStatus", {"namespaces": ["Integrations"]})
    print(f"écoute des notifications pendant {args.watch} s…")
    counts = {}
    deadline = time.time() + args.watch
    nymea.sock.settimeout(3)
    while time.time() < deadline:
        try:
            message = json.loads(nymea._line().decode())
        except Exception:
            continue
        if message.get("notification") == "Integrations.StateChanged":
            key = (message["params"]["thingId"], message["params"]["stateTypeId"])
            counts[key] = counts.get(key, 0) + 1
    if not counts:
        print("  aucune notification — cohérent si aucune valeur n'a changé")
    for (thing_id, state_id), count in sorted(counts.items(), key=lambda x: -x[1]):
        marker = "  <- courbe" if curve_state_ids.get(thing_id) == state_id else ""
        print(f"  {thing_id[:8]}… {state_id[:8]}… : {count}{marker}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
