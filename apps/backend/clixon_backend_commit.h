/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC (Netgate)

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

 */


#ifndef _CLIXON_BACKEND_COMMIT_H_
#define _CLIXON_BACKEND_COMMIT_H_

#define ROLLBACK_NOT_APPLIED        1
#define ROLLBACK_DB_NOT_DELETED     2
#define ROLLBACK_FAILSAFE_APPLIED   4

#define COMMIT_NOT_CONFIRMED "Commit was not confirmed; automatic rollback complete."

enum confirmed_commit_state {
    INACTIVE,       // a confirmed-commit is not in progress
    PERSISTENT,     // a confirmed-commit is in progress and a persist value was given
    EPHEMERAL,      // a confirmed-commit is in progress and a persist value was not given
    ROLLBACK
};

/*
 * Prototypes
 */
/* backend_confirm.c */
int confirmed_commit_init(clicon_handle h);
int confirmed_commit_free(clicon_handle h);
enum confirmed_commit_state confirmed_commit_state_get(clicon_handle h);
uint32_t confirmed_commit_session_id_get(clicon_handle h);
int cancel_rollback_event(clicon_handle h);
int cancel_confirmed_commit(clicon_handle h);
int handle_confirmed_commit(clicon_handle h, cxobj *xe);
int do_rollback(clicon_handle h, uint8_t *errs);
int from_client_cancel_commit(clicon_handle h,  cxobj *xe, cbuf *cbret, void *arg, void *regarg);
int from_client_confirmed_commit(clicon_handle h, cxobj *xe, uint32_t myid, cbuf *cbret);

/* backend_commit.c */
int startup_validate(clicon_handle h, char *db, cxobj **xtr, cbuf *cbret);
int startup_commit(clicon_handle h, char *db, cbuf *cbret);
int candidate_validate(clicon_handle h, char *db, cbuf *cbret);
int candidate_commit(clicon_handle h, cxobj *xe, char *db, cbuf *cbret);

int from_client_commit(clicon_handle h, cxobj *xe, cbuf *cbret, void *arg, void *regarg);
int from_client_discard_changes(clicon_handle h, cxobj *xe, cbuf *cbret, void *arg, void *regarg);
int from_client_validate(clicon_handle h, cxobj *xe, cbuf *cbret, void *arg, void *regarg);
int from_client_restart_one(clicon_handle h, clixon_plugin_t *cp, cbuf *cbret);
int load_failsafe(clicon_handle h, char *phase);

#endif  /* _CLIXON_BACKEND_COMMIT_H_ */
