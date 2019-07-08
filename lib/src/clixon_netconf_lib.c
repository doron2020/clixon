/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 * Netconf library functions. See RFC6241
 * Functions to generate a netconf error message come in two forms: xml-tree and 
 * cbuf. XML tree is preferred.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_err.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_log.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_xml_map.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_netconf_lib.h"

/*! Create Netconf in-use error XML tree according to RFC 6241 Appendix A
 *
 * The request requires a resource that already is in use.
 * @param[out] cb       CLIgen buf. Error XML is written in this buffer
 * @param[in]  type     Error type: "application" or "protocol"
 * @param[in]  message  Error message (will be XML encoded)
 */
int
netconf_in_use(cbuf *cb,
	       char *type,
	       char *message)
{
    int   retval = -1;
    char *encstr = NULL;
    
    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>in-use</error-tag>"
		"<error-severity>error</error-severity>",
		type) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf invalid-value error XML tree according to RFC 6241 Appendix A
 *
 * The request specifies an unacceptable value for one or more parameters.
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  message Error message (will be XML encoded)
 */
int
netconf_invalid_value_xml(cxobj **xret,
			  char   *type,
			  char   *message)
{
    int    retval =-1;
    cxobj *xerr = NULL;
    char  *encstr = NULL;

    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>%s</error-type>"
		     "<error-tag>invalid-value</error-tag>"
		     "<error-severity>error</error-severity>", type) < 0)
	goto done;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (xml_parse_va(&xerr, NULL, "<error-message>%s</error-message>",
			 encstr) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Create Netconf invalid-value error XML tree according to RFC 6241 Appendix A
 *
 * The request specifies an unacceptable value for one or more parameters.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  message Error message (will be XML encoded)
 */
int
netconf_invalid_value(cbuf *cb,
		      char *type,
		      char *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_invalid_value_xml(&xret, type, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf too-big error XML tree according to RFC 6241 Appendix A
 *
 * The request or response (that would be generated) is 
 * too large for the implementation to handle.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "transport", "rpc", "application", "protocol"
 * @param[in]  message Error message (will be XML encoded)
 */
int
netconf_too_big(cbuf *cb,
		char *type,
		char *message)
{
    int     retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>too-big</error-tag>"
		"<error-severity>error</error-severity>",
		type) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf missing-attribute error XML tree according to RFC 6241 App A
 *
 * An expected attribute is missing.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "rpc", "application" or "protocol"
 * @param[in]  info    bad-attribute or bad-element xml
 * @param[in]  message Error message (will be XML encoded)
 */
int
netconf_missing_attribute(cbuf *cb,
			  char *type,
			  char *info,
			  char *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>missing-attribute</error-tag>"
		"<error-info>%s</error-info>"
		"<error-severity>error</error-severity>",
		type, info) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf bad-attribute error XML tree according to RFC 6241 App A
 *
 * An attribute value is not correct; e.g., wrong type,
 * out of range, pattern mismatch.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "rpc", "application" or "protocol"
 * @param[in]  info    bad-attribute or bad-element xml
 * @param[in]  message Error message (will be XML encoded)
 */
int
netconf_bad_attribute(cbuf *cb,
		      char *type,
		      char *info,
		      char *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>bad-attribute</error-tag>"
		"<error-info>%s</error-info>"
		"<error-severity>error</error-severity>",
		type, info) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf unknwon-attribute error XML tree according to RFC 6241 App A
 *
 * An unexpected attribute is present.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "rpc", "application" or "protocol"
 * @param[in]  info    bad-attribute or bad-element xml
 * @param[in]  message Error message
 */
int
netconf_unknown_attribute(cbuf *cb,
			  char *type,
			  char *info,
			  char *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>unknown-attribute</error-tag>"
		"<error-info>%s</error-info>"
		"<error-severity>error</error-severity>",
		type, info) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Common Netconf element XML tree according to RFC 6241 App A
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  tag     Error tag
 * @param[in]  element bad-element xml
 * @param[in]  message Error message (will be XML encoded)
 */
static int
netconf_common_xml(cxobj **xret,
		   char   *type,
		   char   *tag,
		   char   *infotag,
		   char   *element,
		   char   *message)
{
    int    retval =-1;
    cxobj *xerr;
    char  *encstr = NULL;
    
    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>%s</error-type>"
		     "<error-tag>%s</error-tag>"
		     "<error-info><%s>%s</%s></error-info>"
		     "<error-severity>error</error-severity>",
		     type, tag, infotag, element, infotag) < 0)
	goto done;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (xml_parse_va(&xerr, NULL, "<error-message>%s</error-message>",
			 encstr) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}    

/*! Create Netconf missing-element error XML tree according to RFC 6241 App A
 *
 * An expected element is missing.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  info    bad-element xml
 * @param[in]  message Error message
 */
int
netconf_missing_element(cbuf      *cb, 
			char      *type,
			char      *element,
			char      *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_common_xml(&xret, type, "missing-element",
			   "bad-element", element, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf missing-element error XML tree according to RFC 6241 App A
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  element bad-element xml
 * @param[in]  message Error message
 */
int
netconf_missing_element_xml(cxobj **xret,
			    char   *type,
			    char   *element,
			    char   *message)
{
    return netconf_common_xml(xret, type, "missing-element",
			      "bad-element", element, message);
}

/*! Create Netconf bad-element error XML tree according to RFC 6241 App A
 *
 * An element value is not correct; e.g., wrong type, out of range, 
 * pattern mismatch.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  elemnt  Bad element name
 * @param[in]  message Error message
 */
int
netconf_bad_element(cbuf *cb,
		    char *type,
		    char *element,
		    char *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_common_xml(&xret, type, "bad-element",
			   "bad-element",element, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}
int
netconf_bad_element_xml(cxobj **xret,
			char   *type,
			char   *element,
			char   *message)
{
    return netconf_common_xml(xret, type, "bad-element", "bad-element", element, message);
}

/*! Create Netconf unknown-element error XML tree according to RFC 6241 App A
 *
 * An unexpected element is present.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  element Bad element name
 * @param[in]  message Error message
 */
int
netconf_unknown_element(cbuf *cb,
			char *type,
			char *element,
			char *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_common_xml(&xret, type, "unknown-element",
			   "bad-element", element, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf unknown-element error XML tree according to RFC 6241 App A
 *
 * An unexpected element is present.
 * @param[out] xret    XML buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  element Bad element name
 * @param[in]  message Error message
 */
int
netconf_unknown_element_xml(cxobj **xret,
			    char   *type,
			    char   *element,
			    char   *message)
{
    return netconf_common_xml(xret, type, "unknown-element",
			      "bad-element", element, message);
}

/*! Create Netconf unknown-namespace error XML tree according to RFC 6241 App A
 *
 * An unexpected namespace is present.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  info    bad-element or bad-namespace xml
 * @param[in]  message Error message
 */
int
netconf_unknown_namespace(cbuf *cb,
			  char *type,
			  char *namespace,
			  char *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_common_xml(&xret, type, "unknown-namespace",
			   "bad-namespace", namespace, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

int
netconf_unknown_namespace_xml(cxobj **xret,
			      char   *type,
			      char   *namespace,
			      char   *message)
{
    return netconf_common_xml(xret, type, "unknown-namespace",
			      "bad-namespace", namespace, message);
}

/*! Create Netconf access-denied error cbuf according to RFC 6241 App A
 *
 * Access to the requested protocol operation or data model is denied because 
 * authorization failed.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  message Error message
 * @see netconf_access_denied_xml  Same but returns XML tree
 */
int
netconf_access_denied(cbuf *cb,
		      char *type,
		      char *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_access_denied_xml(&xret, type, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf access-denied error XML tree according to RFC 6241 App A
 *
 * Access to the requested protocol operation or data model is denied because
 * authorization failed.
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  message Error message  (will be XML encoded)
 * @code
 *  cxobj *xret = NULL;
 *  if (netconf_access_denied_xml(&xret, "protocol", "Unauthorized") < 0)
 *    err;
 *  xml_free(xret);
 * @endcode
 * @see netconf_access_denied  Same but returns cligen buffer
 */
int
netconf_access_denied_xml(cxobj **xret,
			  char   *type,
			  char   *message)
{
    int    retval =-1;
    cxobj *xerr;
    char  *encstr = NULL;

    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>%s</error-type>"
		     "<error-tag>access-denied</error-tag>"
		     "<error-severity>error</error-severity>", type) < 0)
	goto done;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (xml_parse_va(&xerr, NULL, "<error-message>%s</error-message>",
			 encstr) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Create Netconf lock-denied error XML tree according to RFC 6241 App A
 *
 * Access to the requested lock is denied because the lock is currently held
 * by another entity.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  info    session-id xml
 * @param[in]  message Error message
 */
int
netconf_lock_denied(cbuf *cb,
		    char *info,
		    char *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>protocol</error-type>"
		"<error-tag>lock-denied</error-tag>"
		"<error-info>%s</error-info>"
		"<error-severity>error</error-severity>",
		info) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf resource-denied error XML tree according to RFC 6241 App A
 *
 * Request could not be completed because of insufficient resources.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "transport, "rpc", "application", "protocol"
 * @param[in]  message Error message
 */
int
netconf_resource_denied(cbuf *cb,
			char *type,
			char *message)
{
    int   retval = -1;
    char *encstr = NULL;
    
    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>resource-denied</error-tag>"
		"<error-severity>error</error-severity>",
		type) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf rollback-failed error XML tree according to RFC 6241 App A
 *
 * Request to roll back some configuration change (via rollback-on-error or 
 * <discard-changes> operations) was not completed for some reason.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  message Error message
 */
int
netconf_rollback_failed(cbuf *cb,
			char *type,
			char *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>rollback-failed</error-tag>"
		"<error-severity>error</error-severity>",
		type) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf data-exists error XML tree according to RFC 6241 Appendix A
 *
 * Request could not be completed because the relevant
 * data model content already exists.  For example,
 * a "create" operation was attempted on data that already exists.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  message Error message
 */
int
netconf_data_exists(cbuf      *cb, 
		    char      *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>application</error-type>"
		"<error-tag>data-exists</error-tag>"
		"<error-severity>error</error-severity>") <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf data-missing error XML tree according to RFC 6241 App A
 *
 * Request could not be completed because the relevant data model content 
 * does not exist.  For example, a "delete" operation was attempted on
 * data that does not exist.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  missing_choice  If set, see RFC7950: 15.6 violates mandatiry choice
 * @param[in]  message Error message
 */
int
netconf_data_missing(cbuf *cb,
		     char *missing_choice,
		     char *message)
{
    int   retval = -1;
    cxobj *xret = NULL;

    if (netconf_data_missing_xml(&xret, missing_choice, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf data-missing error XML tree according to RFC 6241 App A
 *
 * Request could not be completed because the relevant data model content 
 * does not exist.  For example, a "delete" operation was attempted on
 * data that does not exist.
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @param[in]  missing_choice  If set, see RFC7950: 15.6 violates mandatiry choice
 * @param[in]  message Error message
 */
int
netconf_data_missing_xml(cxobj **xret,
			 char   *missing_choice,
			 char   *message)
{
    int   retval = -1;
    char *encstr = NULL;
    cxobj *xerr;

    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, 
		     "<error-type>application</error-type>"
		     "<error-tag>data-missing</error-tag>") < 0)
	goto done;
    if (missing_choice) /* NYI: RFC7950: 15.6 <error-path> */
	if (xml_parse_va(&xerr, NULL, 
			 "<error-app-tag>missing-choice</error-app-tag>"
			 "<error-info><missing-choice>%s</missing-choice></error-info>",
			 missing_choice) < 0)
	    goto done;
    if (xml_parse_va(&xerr, NULL, 
		"<error-severity>error</error-severity>") < 0)
	goto done;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (xml_parse_va(&xerr, NULL,
			 "<error-message>%s</error-message>", encstr) < 0)
	    goto done;
    }
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}
    
/*! Create Netconf operation-not-supported error XML according to RFC 6241 App A
 *
 * Request could not be completed because the requested operation is not
 * supported by this implementation.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "application" or "protocol"
 * @param[in]  message Error message
 */
int
netconf_operation_not_supported(cbuf *cb,
				char *type,
				char *message)
{
    int   retval = -1;
    char *encstr = NULL;

    if (cprintf(cb, "<rpc-reply><rpc-error>"
		"<error-type>%s</error-type>"
		"<error-tag>operation-not-supported</error-tag>"
		"<error-severity>error</error-severity>",
		type) <0)
	goto err;
    if (message){
	if (xml_chardata_encode(&encstr, "%s", message) < 0)
	    goto done;
	if (cprintf(cb, "<error-message>%s</error-message>", encstr) < 0)
	    goto err;
    }
    if (cprintf(cb, "</rpc-error></rpc-reply>") <0)
	goto err;
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
 err:
    clicon_err(OE_XML, errno, "cprintf");
    goto done;
}

/*! Create Netconf operation-failed error XML tree according to RFC 6241 App A
 *
 * Request could not be completed because the requested operation failed for
 * some reason not covered by any other error condition.
 * @param[out] cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]  type    Error type: "rpc", "application" or "protocol"
 * @param[in]  message Error message
 * @see netconf_operation_failed_xml  Same but returns XML tree
 */
int
netconf_operation_failed(cbuf  *cb,
			 char  *type,
			 char  *message)
{
    int    retval = -1;
    cxobj *xret = NULL;

    if (netconf_operation_failed_xml(&xret, type, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf operation-failed error XML tree according to RFC 6241 App A
 *
 * Request could not be completed because the requested operation failed for
 * some reason not covered by any other error condition.
 * @param[out] xret    Error XML tree 
 * @param[in]  type    Error type: "rpc", "application" or "protocol"
 * @param[in]  message Error message (will be XML encoded)
 * @code
 *  cxobj *xret = NULL;
 *  if (netconf_operation_failed_xml(&xret, "protocol", "Unauthorized") < 0)
 *    err;
 *  xml_free(xret);
 * @endcode
 * @see netconf_operation_failed  Same but returns cligen buffer
 */
int
netconf_operation_failed_xml(cxobj **xret,
			     char  *type,
			     char  *message)
{
    int   retval =-1;
    cxobj *xerr;
    char  *encstr = NULL;
    
    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>%s</error-type>"
		     "<error-tag>operation-failed</error-tag>"
		     "<error-severity>error</error-severity>", type) < 0)
	goto done;
    if (message){
	 if (xml_chardata_encode(&encstr, "%s", message) < 0)
	     goto done;
	 if (xml_parse_va(&xerr, NULL, "<error-message>%s</error-message>",
			 encstr) < 0)
	goto done;
    }
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Create Netconf malformed-message error XML tree according to RFC 6241 App A
 *
 * A message could not be handled because it failed to be parsed correctly.  
 * For example, the message is not well-formed XML or it uses an
 * invalid character set.
 * @param[out]  cb      CLIgen buf. Error XML is written in this buffer
 * @param[in]   message Error message
 * @note New in :base:1.1
 * @see netconf_malformed_message_xml  Same but returns XML tree
 */
int
netconf_malformed_message(cbuf  *cb,
			  char  *message)
{
    int   retval = -1;
    cxobj *xret = NULL;

    if (netconf_malformed_message_xml(&xret, message) < 0)
	goto done;
    if (clicon_xml2cbuf(cb, xret, 0, 0) < 0)
	goto done;
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Create Netconf malformed-message error XML tree according to RFC 6241 App A
 *
 * A message could not be handled because it failed to be parsed correctly.  
 * For example, the message is not well-formed XML or it uses an
 * invalid character set.
 * @param[out] xret    Error XML tree 
 * @param[in]  message Error message (will be XML encoded)
 * @note New in :base:1.1
 * @code
 *  cxobj *xret = NULL;
 *  if (netconf_malformed_message_xml(&xret, "Unauthorized") < 0)
 *    err;
 *  xml_free(xret);
 * @endcode
 * @see netconf_malformed_message  Same but returns cligen buffer
 */
int
netconf_malformed_message_xml(cxobj **xret,
			      char   *message)
{
    int    retval =-1;
    cxobj *xerr;
    char  *encstr = NULL;
    
    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>rpc</error-type>"
		     "<error-tag>malformed-message</error-tag>"
		     "<error-severity>error</error-severity>") < 0)
	goto done;
     if (message){
	 if (xml_chardata_encode(&encstr, "%s", message) < 0)
	     goto done;
	 if (xml_parse_va(&xerr, NULL, "<error-message>%s</error-message>",
			  encstr) < 0)
	     goto done;
     }
    retval = 0;
 done:
    if (encstr)
	free(encstr);
    return retval;
}

/*! Create Netconf data-not-unique error message according to RFC 7950 15.1
 *
 * A NETCONF operation would result in configuration data where a
 *   "unique" constraint is invalidated.
 * @param[out]  xret   Error XML tree. Free with xml_free after use
 * @param[in]   x      List element containing duplicate
 * @param[in]   cvk    List of comonents in x that are non-unique
 * @see RFC7950 Sec 15.1
 */
int
netconf_data_not_unique_xml(cxobj **xret,
			    cxobj  *x,
			    cvec   *cvk)
{
    int     retval = -1;
    cg_var *cvi = NULL; 
    cxobj  *xi;
    cxobj  *xerr;
    cxobj  *xinfo;
    cbuf   *cb = NULL;
    
    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>protocol</error-type>"
		     "<error-tag>operation-failed</error-tag>"
     		     "<error-app-tag>data-not-unique</error-app-tag>"
		     "<error-severity>error</error-severity>") < 0)
	goto done;
    if (cvec_len(cvk)){
	if ((xinfo = xml_new("error-info", xerr, NULL)) == NULL)
	    goto done;
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	while ((cvi = cvec_each(cvk, cvi)) != NULL){
	    if ((xi = xml_find(x, cv_string_get(cvi))) == NULL)
		continue; /* ignore, shouldnt happen */
	    clicon_xml2cbuf(cb, xi, 0, 0);	
	    if (xml_parse_va(&xinfo, NULL, "<non-unique>%s</non-unique>", cbuf_get(cb)) < 0)
		goto done;
	    cbuf_reset(cb);
	}
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Create Netconf too-many/few-elements err msg according to RFC 7950 15.2/15.3
 *
 * A NETCONF operation would result in configuration data where a
   list or a leaf-list would have too many entries, the following error
 * @param[out] xret    Error XML tree. Free with xml_free after use
 * @param[in]   x        List element containing duplicate
 * @param[in]   max      If set, return too-many, otherwise too-few
 * @see RFC7950 Sec 15.1
 */
int
netconf_minmax_elements_xml(cxobj **xret,
	                    cxobj  *x,
	                    int     max)
{
    int    retval = -1;
    cxobj *xerr;
    
    if (*xret == NULL){
	if ((*xret = xml_new("rpc-reply", NULL, NULL)) == NULL)
	    goto done;
    }
    else if (xml_name_set(*xret, "rpc-reply") < 0)
	goto done;
    if ((xerr = xml_new("rpc-error", *xret, NULL)) == NULL)
	goto done;
    if (xml_parse_va(&xerr, NULL, "<error-type>protocol</error-type>"
		     "<error-tag>operation-failed</error-tag>"
		     "<error-app-tag>too-%s-elements</error-app-tag>"
		     "<error-severity>error</error-severity>"
		     "<error-path>%s</error-path>",
		     max?"many":"few",
		     xml_name(x)) < 0) /* XXX should be xml2xpath */
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Help function: merge - check yang - if error make netconf errmsg 
 * @param[in]     x       XML tree
 * @param[in]     yspec   Yang spec
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @retval       -1       Error (fatal)
 * @retval        0       Statedata callback failed
 * @retval        1       OK
 */
int
netconf_trymerge(cxobj       *x,
		 yang_stmt   *yspec,
    		 cxobj      **xret)
{
    int    retval = -1;
    char  *reason = NULL;
    cxobj *xc;
    
    if (*xret == NULL){
	if ((*xret = xml_dup(x)) == NULL)
	    goto done;
	goto ok;
    }
    if (xml_merge(*xret, x, yspec, &reason) < 0)
	goto done;
    if (reason){
	while ((xc = xml_child_i(*xret, 0)) != NULL)
	    xml_purge(xc);	    
	if (netconf_operation_failed_xml(xret, "rpc", reason)< 0)
	    goto done;
	goto fail;
    }
 ok:
    retval = 1;
 done:
    if (reason)
	free(reason);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Load ietf netconf yang module and set enabled features
 * The features added are (in order):
 *   candidate (8.3)
 *   validate (8.6)
 *   startup (8.7)
 *   xpath (8.9)
 */
int
netconf_module_load(clicon_handle h)
{
    int        retval = -1;
    cxobj     *xc;
    yang_stmt *yspec;

    yspec = clicon_dbspec_yang(h);
    if ((xc = clicon_conf_xml(h)) == NULL){
	clicon_err(OE_CFG, ENOENT, "Clicon configuration not loaded");
	goto done; 
    }
    /* Enable features (hardcoded here) */
    if (xml_parse_string("<CLICON_FEATURE>ietf-netconf:candidate</CLICON_FEATURE>", yspec, &xc) < 0)
	goto done;
    if (xml_parse_string("<CLICON_FEATURE>ietf-netconf:validate</CLICON_FEATURE>", yspec, &xc) < 0)
	goto done;
    if (xml_parse_string("<CLICON_FEATURE>ietf-netconf:xpath</CLICON_FEATURE>", yspec, &xc) < 0)
	goto done;
    /* Load yang spec */
    if (yang_spec_parse_module(h, "ietf-netconf", NULL, yspec)< 0)
	goto done;
    if (yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
	goto done;
    /* YANG module revision change management */
    if (clicon_option_bool(h, "CLICON_XML_CHANGELOG"))
	if (yang_spec_parse_module(h, "clixon-xml-changelog", NULL, yspec)< 0)
	    goto done;
    retval = 0;
 done:
    return retval;
}

/*! Find some sub-child in netconf/xm request.
 * Actually, find a child with a certain name and return its body
 * @param[in]  xn
 * @param[in]  name
 * @retval     db    Name of database
 * @retval     NULL  Not found
 * The following code returns "source"
 * @code
 *   cxobj *xt = NULL;
 *   char  *db;
 *   xml_parse_string("<x><target>source</target></x>", NULL, &xt);
 *   db = netconf_db_find(xt, "target");
 * @endcode
 */
char*
netconf_db_find(cxobj *xn, 
		char  *name)
{
    cxobj *xs; /* source */
    cxobj *xi;
    char  *db = NULL;

    if ((xs = xml_find(xn, name)) == NULL)
	goto done;
    if ((xi = xml_child_i(xs, 0)) == NULL)
	goto done;
    db = xml_name(xi);
 done:
    return db;
}

/*! Generate netconf error msg to cbuf to use in string printout or logs
 * @param[in]  xerr    Netconf error message on the level: <rpc-reply><rpc-error>
 * @param[out] cberr   Translation from netconf err to cbuf. Free with cbuf_free.
 * @retval     0       OK, with cberr set
 * @retval    -1       Error
 * @code
 *     cbuf  *cb = NULL;
 *     if (netconf_err2cb(xerr, &cb) < 0)
 *        err;
 *     printf("%s", cbuf_get(cb));
 * @endcode
 * @see clicon_rpc_generate_error
 */
int
netconf_err2cb(cxobj *xerr,
	       cbuf **cberr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *x;

    if ((cb = cbuf_new()) ==NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((x=xpath_first(xerr, NULL, "error-type"))!=NULL)
	cprintf(cb, "%s ", xml_body(x));
    if ((x=xpath_first(xerr, NULL, "error-tag"))!=NULL)
	cprintf(cb, "%s ", xml_body(x));
    if ((x=xpath_first(xerr, NULL, "error-message"))!=NULL)
	cprintf(cb, "%s ", xml_body(x));
    if ((x=xpath_first(xerr, NULL, "error-info"))!=NULL)
	clicon_xml2cbuf(cb, xml_child_i(x,0), 0, 0);
    *cberr = cb;
    retval = 0;
 done:
    return retval;
}
