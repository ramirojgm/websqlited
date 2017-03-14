/*
	Copyright (C) 2016 Ramiro Jose Garcia Moraga

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WSQLTYPES_H_
#define WSQLTYPES_H_

typedef struct	_WebSQLiteCache WebSQLiteCache;
typedef enum	_WebSQLiteType WebSQLiteType;
typedef struct	_WebSQLiteParam WebSQLiteParam;
typedef struct	_WebSQLiteAction WebSQLiteAction;
typedef struct	_WebSQLiteMimeType WebSQLiteMimeType;
typedef enum _WebSQLiteActionType WebSQLiteActionType;

struct _WebSQLiteMimeType
{
  gchar * ext;
  gchar * mimetype;
};

struct _WebSQLiteCache
{
  gchar * name;
  const gchar * content_type;
  gpointer content;
  gpointer content_gzip;
  gpointer content_deflate;
  gsize content_size;
  gsize content_size_gzip;
  gsize content_size_deflate;
};

enum _WebSQLiteType
{
  WSQL_TYPE_INT,
  WSQL_TYPE_INT64,
  WSQL_TYPE_FLOAT,
  WSQL_TYPE_TEXT,
  WSQL_TYPE_BLOB
};

enum _WebSQLiteActionType
{
  WSQL_ACTION_TEXT,
  WSQL_ACTION_JSON
};

struct _WebSQLiteParam
{
  gchar * 	name;
  WebSQLiteType type;
};

struct _WebSQLiteAction
{
  HttpRequestMethod method;
  WebSQLiteActionType type;
  gchar * name;
  gchar * mimetype;
  WebSQLiteParam ** params;
  gchar ** statements;
  gchar ** exception;
};



#endif /* WSQLTYPES_H_ */
