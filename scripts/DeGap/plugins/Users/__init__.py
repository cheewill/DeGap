import os
import sys
import mmap
import time
import ctypes
import sqlite3
import ConfigParser
import subprocess

from Common import *
from plugins.Users.UsersResource import *
from plugins.Users.UsersConfigs2Perms import *
from plugins.Users.UsersConfigRestrictor import *

def init():
	print('Using \"Users\" plugin.')

def parse_logs(conn, configs):
	print 'Use the \"Auditd\" plugin to parse the logs and create a database.'

def load_metadata(conn, configs):
	print 'Use the \"Auditd\" plugin to load the metadata.'


#------------------------------------------------------------------------------
# Returns list of valid resources.
#------------------------------------------------------------------------------
def get_valid_rsrcs(conn):
	rsrcs = []
	sql = 'SELECT `rid`, `hostname` FROM `Resource`'
	cur = conn.cursor()
	cur.execute(sql)
	row = cur.fetchone()
	while row:
		rid = row[0]
		hostname = row[1]
		if rid != None and hostname != None:
			rsrc = UsersResource( conn, None, rid, hostname )
			rsrcs.append(rsrc)
		row = cur.fetchone()
	cur.close()
	return rsrcs

#------------------------------------------------------------------------------
# Derives database from the one generated by "Auditd" plugin.
#------------------------------------------------------------------------------

def reset_db(db, dbschema):
	# Delete database, then create a new one.
	if os.path.exists(db):
		print 'Removing database \"%s\"...' % db
		os.remove(db)

	if os.path.exists(dbschema) == False:
		print 'Cannot find database schema \"%s\".' % dbschema
		sys.exit(-1)

	print 'Creating database \"%s\" using schema \"%s\"...' % (db, dbschema)
	dbproc = subprocess.Popen(['sqlite3', db], stdin=open(dbschema, 'r') )
	dbproc.communicate()


def load_db(db):
	if not os.path.exists(db):
		print 'Cannot find database \"%s". ' \
				'Please check db configuration in \"%s\". ' \
				'If you are using \"Users\" plugin, you need to use \"Auditd\" plugin '  \
				'first to load the database. The \"Users\" plugin derives its database ' \
				'from the \"Auditd\"\'s database.' \
				% (db, cfgname)
		sys.exit(-1)
	conn = sqlite3.connect(db)
	cur = conn.cursor()
	cur.execute( 'PRAGMA synchronous = OFF' )
	cur.execute( 'PRAGMA journal_mode = MEMORY' )
	cur.close()
	print 'Connected to \"%s\".' % db
	return conn

def update_uid(conn, uid):
	sel_sql = 'SELECT `uid` FROM `user` WHERE `uid`=%d' % (uid)
	ins_sql = 'INSERT INTO `user` VALUES(%d)' % (uid)
	update_or_get_helper(conn, None, sel_sql, ins_sql)

def update_gid(conn, gid):
	sel_sql = 'SELECT `gid` FROM `group` WHERE `gid`=%d' % (gid)
	ins_sql = 'INSERT INTO `group` VALUES(%d)' % (gid)
	update_or_get_helper(conn, None, sel_sql, ins_sql)


def update_user_name(conn, uid, name, gid):
	sel_sql = 'SELECT `unid` FROM `user_name` '\
			'WHERE `uid`=%d AND `name`%s AND `pri_gid`=%d' %\
			(uid, prep_query_str(name), gid)
	ins_sql = 'INSERT INTO `user_name` (`uid`, `name`, `pri_gid`) '\
			'VALUES (%d, %s, %d)' % (uid, prep_ins_str(name), gid)
	#print 'SEL_SQL: %s' % sel_sql
	#print 'INS_SQL: %s' % ins_sql
	unid = update_or_get_helper(conn, None, sel_sql, ins_sql)
	return unid


def update_group_name(conn, gid, name):
	sel_sql = 'SELECT `gnid` FROM `group_name` WHERE `gid`=%d AND `name`%s' %\
			(gid, prep_query_str(name))
	ins_sql = 'INSERT INTO `group_name` (`gid`, `name`) VALUES (%d, %s)' %\
			(gid, prep_ins_str(name))
	gnid = update_or_get_helper(conn, None, sel_sql, ins_sql)
	return gnid


def update_group_membership(conn, unid, gnid):
	sel_sql = 'SELECT `unid` FROM `group_membership` WHERE `unid`%s AND `gnid`%s' %\
			(prep_query_str(unid), prep_query_str(gnid))
	ins_sql = 'INSERT INTO `group_membership` VALUES(%s,%s)' \
			% (prep_ins_str(unid), prep_ins_str(gnid))
	update_or_get_helper(conn, None, sel_sql, ins_sql)


#------------------------------------------------------------------------------
# Get group name from gid. g is the file pointer returned by loadgrp.
#------------------------------------------------------------------------------
def get_group_name(g, gid):
	for k in g.keys():
		for j in g[k]:
			if j.gid == gid:
				return k
	return None


def update_users(conn, configs):
	p = loadpw( configs['passwd'] )
	g = loadgrp( configs['group'] )

	# Update users
	for k in p.keys():
		for j in p[k]:
			update_uid(conn, j.uid)
			update_gid(conn, j.gid)
			unid = update_user_name(conn, j.uid, k, j.gid)
			group_name = get_group_name(g, j.gid)
			#assert group_name != None
			if group_name == None:
				group_name = '!NO-GROUP!'
			gnid = update_group_name(conn, j.gid, group_name)
			update_group_membership(conn, unid, gnid)


	# Update groups
	for k in g.keys():
		for j in g[k]:
			update_gid(conn, j.gid)
			gnid = update_group_name(conn, j.gid, k)
			
			# Update group membership
			for u in j.users:
				if u == '':
					continue
				sql = 'SELECT `unid` FROM `user_name` WHERE name%s' % prep_query_str(u)
				unid = get_helper(conn, None, sql)
				#assert unid != None
				if unid == None:
					continue
				update_group_membership(conn, unid, gnid)



def derive_req_perms_helper(conn, rid, opid, quid, qgid, iuid, igid):
	sel_sql = 'SELECT `aid` FROM `Principal` WHERE `uid`%s AND `gid`%s' % (quid, qgid)
	ins_sql = 'INSERT INTO `Principal` (`uid`,`gid`) VALUES(%s,%s)' % (iuid, igid)
	aid = update_or_get_helper(conn, None, sel_sql, ins_sql)

	sel_sql = 'SELECT `pid` FROM `ReqPerm` WHERE `rid`%s AND `opid`%s AND `aid`%s' %\
			(prep_query_int(rid), prep_query_int(opid), prep_query_int(aid))
	ins_sql = 'INSERT INTO `ReqPerm` (`rid`, `opid`, `aid`) VALUES(%s,%s,%s)' %\
			(prep_ins_int(rid), prep_ins_int(opid), prep_ins_int(aid))
	update_or_get_helper( conn, None, sel_sql, ins_sql )


def derive_req_perms(conn, configs):
	sql = 'SELECT `rid` FROM `Resource` WHERE `hostname`=%s' % (prep_query_str(configs['hostname']))
	rid = get_helper(conn, None, sql)

	sql = 'INSERT INTO `Operation` (`label`) VALUES ("access")'
	opid = update_helper(conn, None, sql)

	cur = conn.cursor()
	cur.execute('SELECT `euid`, `egid` FROM `OpMeta`')
	rows = cur.fetchall()
	for r in rows:
		euid = r[0]
		egid = r[1]

		#
		# Note: euid and egid are inserted as separate principals
		#
		derive_req_perms_helper( conn, rid, opid, prep_query_int(euid), ' IS NULL',\
												 prep_ins_int(euid), 'NULL')
		derive_req_perms_helper( conn, rid, opid, ' IS NULL', prep_query_int(egid),\
												 'NULL', prep_ins_int(egid))

	
	cur.close()


def derive_db(conn, configs):
	reset_db(configs['users-db'], configs['users-dbschema'])
	users_conn = load_db(configs['users-db'])

	users_cur = users_conn.cursor()
	users_cur.execute('ATTACH ? AS `src`', (configs['db'],))
	users_cur.execute('INSERT INTO `OpMeta` SELECT * FROM `src`.`OpMeta`')
	users_cur.execute('INSERT INTO `syscall_execve` SELECT * FROM `src`.`syscall_execve`')
	users_cur.execute('INSERT INTO `syscall_open` SELECT * FROM `src`.`syscall_open`')
	users_cur.execute('INSERT INTO `Process` SELECT * FROM `src`.`Process`')
	users_cur.execute('INSERT INTO `program` SELECT * FROM `src`.`program`')
	users_cur.close()
	users_conn.commit()

	rsrc = UsersResource(users_conn, None, rid=None, hostname=configs['hostname'])
	sql = 'UPDATE `OpMeta` SET `rid`=%s' % prep_ins_int(rsrc.rid)
	update_helper(users_conn, None, sql)
	
	# Compute principals and requested perms.
	rsrc.load_config(configs)
	update_users(users_conn, configs)
	derive_req_perms( users_conn, configs )

	return users_conn


def load_configs(conn, configs, cfg_parser):
	print 'Begin deriving database and loading configs for %s...' % configs['hostname']
	print '='*80
	tic = time.time()

	users_conn = derive_db(conn, configs)
	users_conn.close()

	toc = time.time()
	print '='*80
	elapsed = toc - tic
	print 'Done loading configs for %s. Took %f seconds.' \
			% (configs['hostname'], elapsed)

#------------------------------------------------------------------------------
# Computes granted permissions. conn is actually not used.
#------------------------------------------------------------------------------
def compute_grperms(conn, configs):
	users_conn = load_db(configs['users-db'])

	print 'Begin computing GrPerm for %s...' % configs['hostname']
	print '='*80
	tic = time.time()

	c2p = UsersConfigs2Perms(users_conn, None)
	rsrcs = get_valid_rsrcs(users_conn)
	for rsrc in rsrcs:
		c2p.compute_grperms( rsrc, new=False )

	toc = time.time()
	print '='*80
	elapsed = toc - tic
	print 'Done computing GrPerm for %s. Took %f seconds.' \
			% (configs['hostname'], elapsed)

	users_conn.close()

def restrict_auto(conn, configs):
	users_conn = load_db(configs['users-db'])

	print 'Begin restricting config (auto) for %s...' % configs['hostname']
	print '='*80
	tic = time.time()

	restrictor = UsersConfigRestrictor(users_conn, None)
	rsrcs = get_valid_rsrcs(users_conn)
	for rsrc in rsrcs:
		restrictor.restrict_auto( rsrc )

	toc = time.time()
	print '='*80
	elapsed = toc - tic
	print 'Done restricting config (auto) for %s. Took %f seconds.' \
			% (configs['hostname'], elapsed)

