#!/usr/bin/env bash
# Authentication and authorization and IETF NACM
# External NACM file
# See RFC 8341 A.2
# But replaced ietf-netconf-monitoring with *

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example
# Common NACM scripts
. ./nacm.sh

cfg=$dir/conf_yang.xml
fyang=$dir/nacm-example.yang
fyang2=$dir/clixon-example.yang
nacmfile=$dir/nacmfile

# Define default restconfig config: RESTCONFIG
RESTCONFIG=$(restconf_config user false)

# Note filter out example_backend_nacm.so in CLICON_BACKEND_REGEXP below
cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>${YANG_INSTALLDIR}</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_CLISPEC_DIR>/usr/local/lib/$APPNAME/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_BACKEND_REGEXP>example_backend.so$</CLICON_BACKEND_REGEXP>
  <CLICON_RESTCONF_DIR>/usr/local/lib/$APPNAME/restconf</CLICON_RESTCONF_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_YANG_LIBRARY>false</CLICON_YANG_LIBRARY>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_NACM_MODE>external</CLICON_NACM_MODE>
  <CLICON_NACM_FILE>$nacmfile</CLICON_NACM_FILE>
  <CLICON_NACM_CREDENTIALS>none</CLICON_NACM_CREDENTIALS>
  $RESTCONFIG
</clixon-config>
EOF

cat <<EOF > $fyang
module nacm-example{
  yang-version 1.1;
  namespace "urn:example:nacm";
  prefix nacm;
  import clixon-example {
        prefix ex;
  }
  container authentication {
    presence "To keep this from auto-expanding"; 
    description "Example code for enabling www basic auth and some example 
                     users";
    leaf basic_auth{
        description "Basic user / password authentication as in HTTP basic auth";
        type boolean;
        default true;
    }
    list auth {
        description "user / password entries. Valid if basic_auth=true";
        key user;
        leaf user{
            description "User name";
            type string;
        }
        leaf password{
            description "Password";
            type string;
        }
      }
    }
  leaf x{
    type int32;
    description "something to edit";
  }
}
EOF

cat <<EOF > $fyang2
module clixon-example{
  yang-version 1.1;
  namespace "urn:example:clixon";
  prefix ex;
  /* State data (not config) for the example application*/
  container state {
         config false;
         description "state data for the example application (must be here for example get operation)";
         leaf-list op {
            type string;
         }
  }
    rpc example {
        description "Some example input/output for testing RFC7950 7.14.
                     RPC simply echoes the input for debugging.";
        input {
            leaf x {
                description
                    "If a leaf in the input tree has a 'mandatory' statement with
                   the value 'true', the leaf MUST be present in an RPC invocation.";
                type string;
                mandatory true;
            }
            leaf y {
                description
                 "If a leaf in the input tree has a 'mandatory' statement with the
                  value 'true', the leaf MUST be present in an RPC invocation.";
                type string;
                default "42";
            }
      }
      output {
            leaf x {
                type string;
            }
            leaf y {
                type string;
            }
      }
  }
}
EOF


cat <<EOF > $nacmfile
   <nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
     <enable-nacm>true</enable-nacm>
     <read-default>permit</read-default>
     <write-default>deny</write-default>
     <exec-default>deny</exec-default>

     $NGROUPS

     <rule-list>
       <name>guest-acl</name>
       <group>guest</group>
       <rule>
         <name>deny-ncm</name>
         <module-name>*</module-name>
         <access-operations>*</access-operations>
         <action>deny</action>
         <comment>
             Do not allow guests any access to any information.
         </comment>
       </rule>
     </rule-list>
     <rule-list>
       <name>limited-acl</name>
       <group>limited</group>
       <rule>
         <name>permit-get</name>
         <rpc-name>get</rpc-name>
         <module-name>*</module-name>
         <access-operations>exec</access-operations>
         <action>permit</action>
         <comment>
             Allow get 
         </comment>
       </rule>
       <rule>
         <name>permit-get-config</name>
         <rpc-name>get-config</rpc-name>
         <module-name>*</module-name>
         <access-operations>exec</access-operations>
         <action>permit</action>
         <comment>
             Allow get-config
         </comment>
       </rule>
     </rule-list>

     $NADMIN

   </nacm>
   <x xmlns="urn:example:nacm">0</x>
EOF

new "test params: -f $cfg"

if [ $BE -ne 0 ]; then
    new "kill old backend -zf $cfg "
    sudo clixon_backend -zf $cfg
    if [ $? -ne 0 ]; then
        err
    fi
    sleep 1
    new "start backend -s init -f $cfg -- -s"
    # start new backend
    start_backend -s init -f $cfg -- -s
fi

new "wait backend"
wait_backend

if [ $RC -ne 0 ]; then
    new "kill old restconf daemon"
    stop_restconf_pre

    new "start restconf daemon"
    start_restconf -f $cfg
fi

new "wait restconf"
wait_restconf

new "auth get"
expectpart "$(curl -u andy:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 200" '{"ietf-restconf:data":{"clixon-example:state":{"op":\["41","42","43"\]}'

new "Set x to 0"
expectpart "$(curl -u andy:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" -d '{"nacm-example:x": 0}' $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 201"

new "auth get (no user: access denied)"
expectpart "$(curl $CURLOPTS -X GET -H "Accept: application/yang-data+json" $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 401" '{"ietf-restconf:errors":{"error":{"error-type":"protocol","error-tag":"access-denied","error-severity":"error","error-message":"The requested URL was unauthorized"}}}'

new "auth get (wrong passwd: access denied)"
expectpart "$(curl -u andy:foo $CURLOPTS -X GET $RCPROTO://localhost/restconf/data)" 0 "HTTP/$HVER 401" '{"ietf-restconf:errors":{"error":{"error-type":"protocol","error-tag":"access-denied","error-severity":"error","error-message":"The requested URL was unauthorized"}}}'

new "auth get (access)"
expectpart "$(curl -u andy:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 200" '{"nacm-example:x":0}'

new "admin get nacm"
expectpart "$(curl -u andy:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 200" '{"nacm-example:x":0}'

new "limited get nacm"
expectpart "$(curl -u wilma:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 200" '{"nacm-example:x":0}'

new "guest get nacm"
expectpart "$(curl -u guest:bar $CURLOPTS -X GET $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "admin edit nacm"
expectpart "$(curl -u andy:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" -d '{"nacm-example:x": 1}' $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 204"

new "limited edit nacm"
expectpart "$(curl -u wilma:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" -d '{"nacm-example:x": 2}' $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"default deny"}}}'

new "guest edit nacm"
expectpart "$(curl -u guest:bar $CURLOPTS -X PUT -H "Content-Type: application/yang-data+json" -d '{"nacm-example:x": 3}' $RCPROTO://localhost/restconf/data/nacm-example:x)" 0 "HTTP/$HVER 403" '{"ietf-restconf:errors":{"error":{"error-type":"application","error-tag":"access-denied","error-severity":"error","error-message":"access denied"}}}'

new "cli show conf as admin"
expectpart "$($clixon_cli -1 -U andy -l o -f $cfg show conf)" 0 "x 1;"

new "cli show conf as limited"
expectpart "$($clixon_cli -1 -U wilma -l o -f $cfg show conf)" 0 "x 1;"

new "cli show conf as guest"
expectpart "$($clixon_cli -1 -U guest -l o -f $cfg show conf)" 255 "application access-denied"

new "cli rpc as admin"
expectpart "$($clixon_cli -1 -U andy -l o -f $cfg rpc ipv4)" 0 '<x xmlns="urn:example:clixon">ipv4</x><y xmlns="urn:example:clixon">42</y>'

new "cli rpc as limited"
expectpart "$($clixon_cli -1 -U wilma -l o -f $cfg rpc ipv4)" 255 "access-denied default deny"

new "cli rpc as guest"
expectpart "$($clixon_cli -1 -U guest -l o -f $cfg rpc ipv4)" 255 "access-denied access denied"

if [ $RC -ne 0 ]; then
    new "Kill restconf daemon"
    stop_restconf
fi

if [ $BE -ne 0 ]; then
    new "Kill backend"
    # Check if premature kill
    pid=$(pgrep -u root -f clixon_backend)
    if [ -z "$pid" ]; then
        err "backend already dead"
    fi
    # kill backend
    stop_backend -f $cfg
fi

# Set by restconf_config
unset RESTCONFIG

rm -rf $dir

new "endtest"
endtest
