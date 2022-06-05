/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Kristofer Hallin

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  * See RFC 6643
  * Extensions are grouped in some categories, the one I have seen are, example:
  * 1. leaf
  *      smiv2:max-access "read-write";
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1.1";
  *      smiv2:defval "42"; (not always)
  * 2. container, list
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1";	
  * 3. module level
  *      smiv2:alias "netSnmpExamples" {
  *        smiv2:oid "1.3.6.1.4.1.8072.2";
  *
  * SNMP messages:
  * 160 MODE_GETNEXT / SNMP_MSG_GET
  * 161 MODE_GET / SNMP_MSG_GETNEXT
  * 0   MODE_SET_RESERVE1
  * 1   MODE_SET_RESERVE2
  * 2   MODE_SET_ACTION
  * 3   MODE_SET_COMMIT
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <assert.h>

/* net-snmp */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "snmp_lib.h"
#include "snmp_register.h"
#include "snmp_handler.h"

#define IETF_YANG_SMIV2_NS "urn:ietf:params:xml:ns:yang:ietf-yang-smiv2"


/*! Parse smiv2 extensions for YANG leaf
 * Typical leaf:
 *      smiv2:oid "1.3.6.1.4.1.8072.2.1.1";
 *      smiv2:max-access "read-write";
 *      smiv2:defval "42"; (optional)
 * @param[in]  h    Clixon handle
 * @param[in]  ys   Mib-Yang node
 * @param[in]  cvk  Vector of key/index values. NB: not for scalars, only tables
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
mibyang_leaf_register(clicon_handle h,
		      yang_stmt    *ys,
		      cvec         *cvk)
{
    int                           retval = -1;
    netsnmp_handler_registration *nhreg = NULL;
    netsnmp_mib_handler          *handler;
    int                           ret;
    char                         *modes_str = NULL;
    char                         *default_str = NULL;
    char                         *oidstr = NULL;
    oid                           oid1[MAX_OID_LEN] = {0,};
    size_t                        sz1 = MAX_OID_LEN;
    int                           modes;
    char                         *name;
    clixon_snmp_handle           *sh;
    cg_var                       *cvi;
    cbuf                         *cboid = NULL;

    /* Get OID from leaf */
    if (yang_extension_value(ys, "oid", IETF_YANG_SMIV2_NS, NULL, &oidstr) < 0)
	goto done;
    if (oidstr == NULL)
	goto ok;
    /* Append sub-keys to original oidstr, use cligen-buf
     */
    if ((cboid = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cboid, "%s", oidstr);
    cvi = NULL;
    while ((cvi = cvec_each(cvk, cvi)) != NULL)
	cprintf(cboid, ".%s", cv_string_get(cvi));
    if (snmp_parse_oid(cbuf_get(cboid), oid1, &sz1) == NULL){
	clicon_err(OE_XML, 0, "snmp_parse_oid(%s)", cbuf_get(cboid));
	//	goto done;
	goto ok; // XXX skip
    }
    if (yang_extension_value(ys, "max-access", IETF_YANG_SMIV2_NS, NULL, &modes_str) < 0)
	goto done;
    /* Only for sanity check of types initially to fail early */
    if (type_yang2asn1(ys, NULL, 0) < 0)
	goto done;

    /* Get modes (access) read-only, read-write, not-accessible, accessible-for-notify
     */
    if (modes_str == NULL)
	goto ok;
    modes = snmp_access_str2int(modes_str);

    /* SMI default value, How is this different from yang defaults?
     */
    if (yang_extension_value(ys, "defval", IETF_YANG_SMIV2_NS, NULL, &default_str) < 0)
    	goto done;

    name = yang_argument_get(ys);

    if ((handler = netsnmp_create_handler(name, clixon_snmp_scalar_handler)) == NULL){
	clicon_err(OE_XML, errno, "netsnmp_create_handler");
	goto done;
    }

    /* Userdata to pass around in netsmp callbacks 
     * XXX: not deallocated
     */
    if ((sh = malloc(sizeof(*sh))) == NULL){
       clicon_err(OE_UNIX, errno, "malloc");
       goto done;
    }
    memset(sh, 0, sizeof(*sh));
    sh->sh_h = h;
    sh->sh_ys = ys;
    memcpy(sh->sh_oid, oid1, sizeof(oid1));
    sh->sh_oidlen = sz1;
    sh->sh_default = default_str;
    if (cvk &&
	(sh->sh_cvk = cvec_dup(cvk)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_dup");
	goto done;
    }
    if ((nhreg = netsnmp_handler_registration_create(name, handler,
						     oid1, sz1,
						     modes)) == NULL){
	clicon_err(OE_XML, errno, "netsnmp_handler_registration_create");
	netsnmp_handler_free(handler);
	goto done;
    }
    /* Register our application data and how to free it */
    handler->myvoid = (void*)sh;
    handler->data_clone = snmp_handle_clone;
    handler->data_free = snmp_handle_free;

    /* 
     * XXX: nhreg->agent_data
     */
    if ((ret = netsnmp_register_instance(nhreg)) != SNMPERR_SUCCESS){
	/* XXX Failures are MIB_REGISTRATION_FAILED and MIB_DUPLICATE_REGISTRATION. */
	clicon_err(OE_SNMP, ret, "netsnmp_register_instance");
	goto done;
    }
    clicon_debug(1, "%s %s registered", __FUNCTION__, cbuf_get(cboid));
 ok:
    retval = 0;
 done:
    if (cboid)
	cbuf_free(cboid);
    return retval;
}

/*! Register table entry handler itself (not column/row leafs) 
 *
 * Parse smiv2 extensions for YANG container/list 
 *
  * Typical table:
  *   container x {
  *      smiv2:oid "1.3.6.1.4.1.8072.2.2.1";
  *      list y{
  *      
  *      }
  *   }
 * @param[in]  h    Clixon handle
 * @param[in]  ys   Mib-Yang node (container)
 * @param[in]  yl   Mib-Yang node (list)
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
mibyang_table_register(clicon_handle h,
		       yang_stmt    *ys,
		       yang_stmt    *ylist)
{
    int                              retval = -1;
    netsnmp_handler_registration    *nhreg;
    char                            *oidstr = NULL;
    oid                              oid1[MAX_OID_LEN] = {0,};
    size_t                           sz1 = MAX_OID_LEN;
    char                            *name;
    clixon_snmp_handle              *sh;
    int                              ret;
    netsnmp_mib_handler             *handler;
    netsnmp_table_registration_info *table_info = NULL;
    cvec                            *cvk = NULL; /* vector of index keys */
    cg_var                          *cvi;
    char                            *keyname;
    yang_stmt                       *yleaf;
    int                              asn1type;

    /* Get OID from parent container  */
    if (yang_extension_value(ys, "oid", IETF_YANG_SMIV2_NS, NULL, &oidstr) < 0)
	goto done;
    if (oidstr == NULL)
	goto ok;
    if (snmp_parse_oid(oidstr, oid1, &sz1) == NULL){
	clicon_err(OE_XML, errno, "snmp_parse_oid");
	goto done;
    }
    name = yang_argument_get(ys);

    /* Userdata to pass around in netsmp callbacks 
     * XXX: not deallocated
     */
    if ((sh = malloc(sizeof(*sh))) == NULL){
       clicon_err(OE_UNIX, errno, "malloc");
       goto done;
    }
    memset(sh, 0, sizeof(*sh));
    sh->sh_h = h;
    sh->sh_ys = ys;

    memcpy(sh->sh_oid, oid1, sizeof(oid1));
    sh->sh_oidlen = sz1;

    if ((handler = netsnmp_create_handler(name, clixon_snmp_table_handler)) == NULL){
	clicon_err(OE_XML, errno, "netsnmp_create_handler");
	goto done;
    }
    if ((nhreg = netsnmp_handler_registration_create(name, handler,
						     oid1, sz1,
						     HANDLER_CAN_RWRITE)) == NULL){
	clicon_err(OE_XML, errno, "netsnmp_handler_registration_create");
	netsnmp_handler_free(handler);
	goto done;
    }
    /* Register our application data and how to free it */
    handler->myvoid =(void*)sh;
    handler->data_clone = snmp_handle_clone;
    handler->data_free = snmp_handle_free;
    
    /* See netsnmp_register_table_data_set */
    if ((table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info)) == NULL){
	clicon_err(OE_UNIX, errno, "SNMP_MALLOC_TYPEDEF");
	goto done;
    }
    /* Keys, go through keys */
    if ((cvk = yang_cvec_get(ylist)) == NULL){
	clicon_err(OE_YANG, 0, "No keys");
	goto done;
    }
    cvi = NULL;
    /* Iterate over individual keys  */
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	keyname = cv_string_get(cvi);
	if ((yleaf = yang_find(ylist, Y_LEAF, keyname)) == NULL){
	    clicon_err(OE_XML, 0, "List statement \"%s\" has no key leaf \"%s\"", 
		       yang_argument_get(ylist), keyname);
	    goto done;
	}
	if (type_yang2asn1(yleaf, &asn1type, 0) < 0)
	    //	    goto done;
	    goto ok; // XXX skip
	if (snmp_varlist_add_variable(&table_info->indexes,
				      NULL, // oid name
				      0,    // oid len
				      asn1type,
				      NULL, // value
				      0) == NULL){
	    clicon_err(OE_XML, errno, "snmp_varlist_add_variable");
	    goto done;
	}
    }
    table_info->min_column = cvec_len(cvk);

    /* Count columns */
    yleaf = NULL;
    table_info->max_column = 0;    
    while ((yleaf = yn_each(ylist, yleaf)) != NULL) {
	if (yang_keyword_get(yleaf) == Y_LEAF)
	    table_info->max_column++;    
    }
    if ((ret = netsnmp_register_table(nhreg, table_info)) != SNMPERR_SUCCESS){
	clicon_err(OE_SNMP, ret, "netsnmp_register_table");
	goto done;
    }
    sh->sh_table_info = table_info;
    clicon_debug(1, "%s %s registered", __FUNCTION__, oidstr);
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Register table sub-oid:s of existing entries in clixon
 * This assumes a table contains a set of keys and a list of leafs only
 * The function makes a query to the datastore and registers all table entries that
 * currently exists. This means it registers for a static table. If new rows or columns
 * are created or deleted this will not change the OID registration.
 * That is, the table registration is STATIC
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Mib-Yang node (container)
 * @param[in]  ylist Mib-Yang node (list)
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
mibyang_table_traverse_static(clicon_handle h,
			      yang_stmt    *ys,
			      yang_stmt    *ylist)
{
    int        retval = -1;
    cvec      *nsc = NULL;
    char      *xpath = NULL;
    cxobj     *xt = NULL;
    cxobj     *xerr;
    cxobj     *xtable;
    cxobj     *xrow;
    cxobj     *xcol;
    yang_stmt *y;
    cvec      *cvk0;
    cg_var    *cv0;
    cvec      *cvk = NULL; /* vector of index keys */
    cg_var    *cv;
    int        i;
    cxobj     *xi;
    
    clicon_debug(1, "%s %s", __FUNCTION__, yang_argument_get(ys));
    if (xml_nsctx_yang(ys, &nsc) < 0)
        goto done;
    if (yang2xpath(ys, NULL, &xpath) < 0)
        goto done;
    if (clicon_rpc_get(h, xpath, nsc, CONTENT_ALL, -1, &xt) < 0)
        goto done;
    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "clicon_rpc_get", NULL);
        goto done;
    }
    if ((xtable = xpath_first(xt, nsc, "%s", xpath)) != NULL) {
	/* Make a clone of key-list, but replace names with values */
	if ((cvk0 = yang_cvec_get(ylist)) == NULL){
	    clicon_err(OE_YANG, 0, "No keys");
	    goto done;
	}
	xrow = NULL;
	while ((xrow = xml_child_each(xtable, xrow, CX_ELMNT)) != NULL) {
	    if (cvk){
		cvec_free(cvk);
		cvk = NULL;
	    }
	    if ((cvk = cvec_dup(cvk0)) == NULL){
		clicon_err(OE_UNIX, errno, "cvec_dup");
		goto done;
	    }
	    for (i=0; i<cvec_len(cvk0); i++){
		cv0 = cvec_i(cvk0, i); 
		cv = cvec_i(cvk, i); 
		if ((xi = xml_find_type(xrow, NULL, cv_string_get(cv0), CX_ELMNT)) == NULL)
		    break;
		if (snmp_body2oid(xi, cv) < 0)
		    goto done;
	    }
	    if (i<cvec_len(cvk0))
		continue; /* skip row, not all indexes */
	    xcol = NULL;
	    while ((xcol = xml_child_each(xrow, xcol, CX_ELMNT)) != NULL) {
		if ((y = xml_spec(xcol)) == NULL)
		    continue;
		if (mibyang_leaf_register(h, y, cvk) < 0) 
		    goto done;
	    }
	}
    }
    retval = 0;
 done:
    if (xpath)
	free(xpath);
    if (cvk)
        cvec_free(cvk);
    if (xt)
        xml_free(xt);
    if (nsc)
        xml_nsctx_free(nsc);    
    return retval;
}

/*! Traverse mib-yang tree, identify scalars and tables, register OID and callbacks
 *
 * The tree is traversed depth-first, which at least guarantees that a parent is
 * traversed before a child.
  * Extensions are grouped in some categories, the one I have seen are, example:
  * 1. leaf
  *      smiv2:max-access "read-write";
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1.1";
  *      smiv2:defval "42"; (not always)
  * 2. container, list
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1";	
  * 3. module level
  *      smiv2:alias "netSnmpExamples" {
  *        smiv2:oid "1.3.6.1.4.1.8072.2";

 * @param[in]  h     Clixon handle
 * @param[in]  yn    yang node
 * @param[in]  table Yang node is within table
 * @retval     0     OK, all nodes traversed
 * @retval    -1     Error, aborted at first error encounter
 */
static int
mibyang_traverse(clicon_handle h,
		 yang_stmt    *yn)
{
    int        retval = -1;
    yang_stmt *ys = NULL;
    yang_stmt *yp;
    int        ret;
	
    clicon_debug(1, "%s %s", __FUNCTION__, yang_argument_get(yn));
    switch(yang_keyword_get(yn)){
    case Y_LEAF:
	if (mibyang_leaf_register(h, yn, NULL) < 0)
	    goto done;
	break;
    case Y_CONTAINER: /* See list case */
	break;
    case Y_LIST: /* If parent is container -> identify as table */
	yp = yang_parent_get(yn);
	if (yang_keyword_get(yp) == Y_CONTAINER){
	    /* Register table entry handler itself (not column/row leafs) */
	    if (mibyang_table_register(h, yp, yn) < 0)
		goto done;
	    /* Register table sub-oid:s of existing entries in clixon */
	    if (mibyang_table_traverse_static(h, yp, yn) < 0)
		goto done;
	    goto ok;
	}
	break;
    default:
	break;
    }
    /* Traverse data nodes in tree (module is special case */
    ys = NULL;
    while ((ys = yn_each(yn, ys)) != NULL) {
	if (!yang_schemanode(ys))
	    continue;
	if ((ret = mibyang_traverse(h, ys)) < 0)
	    goto done;
	if (ret > 0){
	    retval = ret;
	    goto done;
	}
    }
 ok:
    retval = 0;
 done:
    return retval;
}    

/*! Init mib-translated yangs and register callbacks by traversing the yang
 *
 * @þaram[in]  h  Clixon handle
 * @retval     0  OK
 * @retval    -1  Error
 */
int
clixon_snmp_traverse_mibyangs(clicon_handle h)
{
    int        retval = -1;
    char      *modname;
    cxobj     *x;
    yang_stmt *yspec;
    yang_stmt *ymod;

    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_FATAL, 0, "No DB_SPEC");
	goto done;
    }
    /* Loop over clixon configuration file to find all CLICON_SNMP_MIB, and
     * then loop over all those MIBs to register OIDs with netsnmp
     */
    x = NULL;
    while ((x = xml_child_each(clicon_conf_xml(h), x, CX_ELMNT)) != NULL) {
	if (strcmp(xml_name(x), "CLICON_SNMP_MIB") != 0)
	    continue;
	if ((modname = xml_body(x)) == NULL)
	    continue;
	clicon_debug(1, "%s %s: \"%s\"", __FUNCTION__, xml_name(x), modname);
	/* Note, here we assume the Yang is loaded by some other mechanism and
	 * error if it not found.
	 * Alternatively, that YANG could be loaded.
	 * Problem is, if clixon_snmp has not loaded it, has backend done it?
	 * What happens if backend has not loaded it?
	 */
	if ((ymod = yang_find(yspec, Y_MODULE, modname)) == NULL){
	    clicon_err(OE_YANG, 0, "Mib-translated-yang %s not loaded", modname);
	    goto done;
	}
	/* Recursively traverse the mib-yang to find extensions */
	if (mibyang_traverse(h, ymod) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}
