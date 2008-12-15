# Copyright (c) 2001, Stanford University
# All rights reserved.
#
# See the file LICENSE.txt for information on redistributing this software.

import sys, string

sys.path.append( "../glapi_parser" )
import apiutil


if len(sys.argv) != 2:
	print >> sys.stderr, "Usage: %s <filename>" % sys.argv[0]
	sys.exit(-1)

file = open(sys.argv[1])

print "/* THIS FILE IS AUTOGENERATED FROM %s BY pack_swap.py */\n\n" % sys.argv[1]

for line in file.readlines():
	line = line.rstrip()
	if line.find( "crPackAlloc" ) != -1 or line.find( "crPackFree" ) != -1:
		print line
		continue
	elif line.find( "crPack" ) != -1:
		fun_index = line.find( "crPack" )
		paren_index = line.find( "(", fun_index )
		space_index = line.find( " ", fun_index )
		quote_index = line.find( '"', fun_index )
		if paren_index == -1:
			paren_index = 1000000; # HACK HACK
		if space_index == -1:
			space_index = 1000000; # HACK HACK
		if quote_index == -1:
			quote_index = 1000000; # HACK HACK
		the_index = min( min( paren_index, space_index ), quote_index )
		print "%sSWAP%s" % (line[:the_index], line[the_index:])
	elif line.find( "WRITE_DATA" ) != -1:
		lparen_index = line.find( "(" )
		rparen_index = line.rfind( ")" )
		args = map( string.strip, line[lparen_index+1:rparen_index].split( "," ) )
		indentation = line[:line.find( "WRITE_DATA" )]
		if apiutil.sizeof(args[1]) == 1:
			print "%sWRITE_DATA( %s, %s, %s );" % (indentation, args[0], args[1], args[2])
		elif apiutil.sizeof(args[1]) == 2:
			print "%sWRITE_DATA( %s, %s, SWAP16(%s) );" % (indentation, args[0], args[1], args[2])
		elif args[1] == 'GLfloat' or args[1] == 'GLclampf':
			print "%sWRITE_DATA( %s, GLuint, SWAPFLOAT(%s) );" % (indentation, args[0], args[2])
		elif apiutil.sizeof(args[1]) == 4:
			print "%sWRITE_DATA( %s, %s, SWAP32(%s) );" % (indentation, args[0], args[1], args[2])
		else:
			print >> sys.stderr, "UNKNOWN TYPE FOR WRITE_DATA: %s" % args[1]
			sys.exit(-1)
	elif line.find( "WRITE_DOUBLE" ) != -1:
		print line.replace( "WRITE_DOUBLE", "WRITE_SWAPPED_DOUBLE" )
	else:
		print line
