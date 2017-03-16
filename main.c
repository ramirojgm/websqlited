/*
	Copyright (C) 2017 Ramiro Jose Garcia Moraga

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wsql.h"


GMutex db_mutex = G_STATIC_MUTEX_INIT;
GMutex cache_mutex = G_STATIC_MUTEX_INIT;
GMutex action_mutex = G_STATIC_MUTEX_INIT;

sqlite3 * db = NULL;
GList *	  cache_store = NULL;
GList *	  type_store = NULL;
GList *	  action_store = NULL;

gchar *   target_dir = NULL;

const gchar *		websqlite_get_mimetype(const gchar * filename);
void			websqlite_request(GWebSocketService * service,HttpRequest * request,GSocketConnection * connection,gpointer data);
gpointer		websqlite_content(HttpRequest *request,HttpResponse * response,gsize * content_size);
void			websqlite_parse(const gchar * action_base,const gchar *  filename);
WebSQLiteAction * 	websqlite_get_action(const gchar * name);
gchar *			websqlite_action_exec(WebSQLiteAction * action,HttpRequest * request,HttpResponse * response,GSocketConnection * connection,gsize * result_size);

gint main (gint argc,gchar * argv[])
{
  const gchar * name = NULL;
  //startup mimetypes
  FILE * mimetype_file = fopen("mimetype.lst","r");
  if(mimetype_file)
    {
      gchar ext[50];
      gchar mimetype[256];
      while(!feof(mimetype_file))
	{
	  if(fscanf(mimetype_file,"%s %s",ext,mimetype) <= 0) break;
	  WebSQLiteMimeType * type = g_new0(WebSQLiteMimeType,1);
	  type->ext = g_strdup(ext);
	  type->mimetype = g_strdup(mimetype);
	  type_store = g_list_append(type_store,type);
	}
      fclose(mimetype_file);
    }

  //startup wsql
  GDir * wsql_dir = g_dir_open("wsql",0,NULL);
  if(wsql_dir)
    {
      while((name = g_dir_read_name(wsql_dir)))
	{
	  if(g_str_has_suffix(name,".wsql"))
	    {
	      gchar * fullname = g_build_filename(g_get_current_dir(),"wsql",name,NULL);
	      gchar * base = g_strndup(name,g_utf8_strlen(name,256) - 5);
	      websqlite_parse(base,fullname);
	      g_free(fullname);
	    }
	}
      g_dir_close(wsql_dir);
    }

  //startup db
  if(sqlite3_open("data/main.db",&db) == SQLITE_OK)
    {
      GMainLoop * main_loop = g_main_loop_new(NULL,FALSE);
      GWebSocketService * service = g_websocket_service_new(30);
      GDir * data_dir = g_dir_open("data/",0,NULL);
      if(data_dir)
	{

	  while((name = g_dir_read_name(data_dir)))
	    {
	      if(g_str_has_suffix(name,".db"))
		{
		  if(g_strcmp0(name,"main.db"))
		    {
		      gchar * alias = g_strndup(name,g_utf8_strlen(name,256) - 3);
		      gchar * attach = g_strdup_printf("attach 'data/%s' as %s;",name,alias);
		      sqlite3_exec(db,attach,NULL,NULL,NULL);
		      g_free(alias);
		      g_free(attach);
		    }
		}
	    }
	  g_dir_close(data_dir);
	}
      g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service),8080,G_OBJECT(service),NULL);
      g_socket_service_start(G_SOCKET_SERVICE(service));
      g_signal_connect(G_OBJECT(service),"request",G_CALLBACK(websqlite_request),main_loop);
      g_main_loop_run(main_loop);
    }

  return 0;
}


void
websqlite_request(GWebSocketService * service,HttpRequest * request,GSocketConnection * connection,gpointer data)
{
  HttpResponse * response = http_response_new(HTTP_RESPONSE_NOT_FOUND,1.1);
  GOutputStream * output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  gboolean autorized = TRUE;
  sqlite3_stmt * result = NULL;
  gchar * content = NULL;
  gsize content_size = 0;

  g_mutex_lock(&db_mutex);
  autorized = TRUE;
  if(sqlite3_prepare(db,"SELECT (user|| ':'|| password) AS credential FROM www.credentials",-1,&result,NULL) == SQLITE_OK)
    {
      autorized = FALSE;
      if(http_package_is_set(HTTP_PACKAGE(request),"Authorization"))
	{
	  const gchar * remote_credential = http_package_get_string(HTTP_PACKAGE(request),"Authorization",NULL) + 6;
	  while(sqlite3_step(result) == SQLITE_ROW)
	    {
	      gchar * stored_credential = g_base64_encode(sqlite3_column_text(result,0),sqlite3_column_bytes(result,0));
	      gboolean equal = g_strcmp0(remote_credential,stored_credential) == 0;
	      g_free(stored_credential);
	      if(equal)
		{
		  autorized = TRUE;
		  break;
		}
	    }
	}
      sqlite3_finalize(result);
    }
  g_mutex_unlock(&db_mutex);

  if(autorized)
    {
      if(g_utf8_validate(http_request_get_query(request),-1,NULL))
	{
	  WebSQLiteAction * action = websqlite_get_action(http_request_get_query(request));
	  if(action)
	  {
	      if(action->method == http_request_get_method(request))
		{
		  if((action->method == HTTP_REQUEST_METHOD_POST) && (!http_package_is_set(HTTP_PACKAGE(request),"Content-Length")))
		    {
		      http_response_set_code(response,HTTP_RESPONSE_LENGTH_REQUIRED);
		    }
		  else
		    {
		      g_mutex_lock(&db_mutex);
		      content = websqlite_action_exec(action,request,response,connection,&content_size);
		      g_mutex_unlock(&db_mutex);
		    }
		}
	      else
		{
		  http_response_set_code(response,HTTP_RESPONSE_METHOD_NOT_ALLOWED);
		}
	  }
	  else
	  {
	    content = websqlite_content(request,response,&content_size);
	  }
	}
      else
	{
	  http_response_set_code(response,HTTP_RESPONSE_BAD_REQUEST);
	}
    }
  else
    {
      http_response_set_code(response,HTTP_RESPONSE_UNAUTHORIZED);
      http_package_set_string(HTTP_PACKAGE(response),"WWW-Authenticate","Basic realm=\"Credential\"",24);
    }

  http_package_write_to_stream(HTTP_PACKAGE(response),output,NULL,NULL,NULL);
  if(content)
    {
      g_output_stream_write_all(output,content,content_size,NULL,NULL,NULL);
      g_free(content);
    }
  g_object_unref(response);
  g_mutex_lock(&db_mutex);
  g_mutex_unlock(&db_mutex);
  g_io_stream_close(G_IO_STREAM(connection),NULL,NULL);
}

gsize
websqlite_get_file_size(const gchar * filename)
{
  gsize result = 0;
  FILE * file_sized = fopen(filename,"r");
  fseek(file_sized,0,SEEK_END);
  result = ftell(file_sized);
  fclose(file_sized);
  return result;
}

JsonNode *
websqlite_parse_params(gsize size,GInputStream * stream)
{
  gchar * playload = g_new0(gchar,size + 1);
  gsize playload_size = 0;
  g_input_stream_read_all(stream,playload,size,&playload_size,NULL,NULL);
  JsonParser * parser = json_parser_new();
  json_parser_load_from_data(parser,playload,playload_size,NULL);
  g_free(playload);
  JsonNode * root = json_node_ref(json_parser_get_root(parser));
  g_object_unref(parser);
  return root;
}

gpointer
websqlite_compress(GZlibCompressorFormat format,gpointer data,gsize size,gsize * new_size)
{
  GZlibCompressor * compressor = g_zlib_compressor_new(format,9);
  GOutputStream * output = g_memory_output_stream_new_resizable();
  GOutputStream * compressor_stream = g_converter_output_stream_new(output,G_CONVERTER(compressor));
  gpointer data_result = NULL;
  g_output_stream_write_all(compressor_stream,data,size,NULL,NULL,NULL);
  g_output_stream_flush(compressor_stream,NULL,NULL);
  g_output_stream_close(compressor_stream,NULL,NULL);
  g_output_stream_close(output,NULL,NULL);
  data_result = g_memory_output_stream_steal_data(G_MEMORY_OUTPUT_STREAM(output));
  *new_size = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(output));
  g_object_unref(compressor);
  g_object_unref(output);
  g_object_unref(compressor_stream);
  return data_result;
}

gpointer
websqlite_content(HttpRequest * request,HttpResponse * response,gsize * content_size)
{
  gchar * result = NULL;
  const gchar * encoding = http_package_get_string(HTTP_PACKAGE(request),"Accept-Encoding",NULL);
  g_mutex_lock(&cache_mutex);
  for(GList *  iter = g_list_first(cache_store);iter;iter = g_list_next(iter))
    {
      WebSQLiteCache * cached = (WebSQLiteCache *)iter->data;
      if(g_strcmp0(http_request_get_query(request),cached->name) == 0)
	{
	  http_response_set_code(response,HTTP_RESPONSE_OK);
	  if(cached->content_type)
	    http_package_set_string(HTTP_PACKAGE(response),"Content-Type",cached->content_type,g_utf8_strlen(cached->content_type,256));

	  if(g_strstr_len(encoding,-1,"gzip"))
	    {
	      http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","gzip",4);
	      http_package_set_int64(HTTP_PACKAGE(response),"Content-Length",cached->content_size_gzip);
	      result = (gchar*)g_memdup(cached->content_gzip,cached->content_size_gzip);
	      *content_size = cached->content_size_gzip;
	    }
	  else if(g_strstr_len(encoding,-1,"deflate"))
	    {
	      http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","deflate",7);
	      http_package_set_int64(HTTP_PACKAGE(response),"Content-Length",cached->content_size_deflate);
	      result = (gchar*)g_memdup(cached->content_deflate,cached->content_size_deflate);
	      *content_size = cached->content_size_deflate;
	    }
	  else
	    {
	      http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","identity",8);
	      http_package_set_int64(HTTP_PACKAGE(response),"Content-Length",cached->content_size);
	      result = (gchar*)g_memdup(cached->content,cached->content_size);
	      *content_size = cached->content_size;
	    }
	}
    }
  g_mutex_unlock(&cache_mutex);

  if(!result)
    {
      gchar * real_path = NULL;
      if(g_str_has_suffix(http_request_get_query(request),"/"))
	real_path = g_build_filename(g_get_current_dir(),"public",http_request_get_query(request),"index.html",NULL);
      else
	real_path = g_build_filename(g_get_current_dir(),"public",http_request_get_query(request),NULL);
      if(g_path_is_absolute(real_path))
	{
	  if(g_file_test(real_path,G_FILE_TEST_EXISTS))
	    {
	      gsize file_size = websqlite_get_file_size(real_path);
	      gchar * file_content = NULL;
	      if((file_size <= WEBSQLITE_MAX_FILE_CACHE_SIZE) || (file_size <= WEBSQLITE_MAX_FILE_SIZE))
		{
		  GError * error = NULL;
		  if(g_file_get_contents(real_path,&file_content,NULL,&error))
		    {
		      http_response_set_code(response,HTTP_RESPONSE_OK);
		      if(file_size <= WEBSQLITE_MAX_FILE_CACHE_SIZE)
			{
			  WebSQLiteCache * new_cached = g_new0(WebSQLiteCache,1);
			  new_cached->name = g_strdup(http_request_get_query(request));
			  new_cached->content = g_memdup(file_content,file_size);
			  new_cached->content_size = file_size;
			  new_cached->content_gzip = websqlite_compress(G_ZLIB_COMPRESSOR_FORMAT_GZIP,file_content,file_size,&(new_cached->content_size_gzip));
			  new_cached->content_deflate = websqlite_compress(G_ZLIB_COMPRESSOR_FORMAT_ZLIB,file_content,file_size,&(new_cached->content_size_deflate));
			  new_cached->content_type = websqlite_get_mimetype(real_path);
			  g_mutex_lock(&cache_mutex);
			  if(new_cached->content_type)
			    http_package_set_string(HTTP_PACKAGE(response),"Content-Type",new_cached->content_type,g_utf8_strlen(new_cached->content_type,256));

			  if(g_strstr_len(encoding,-1,"gzip"))
			    {
			      http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","gzip",4);
			      http_package_set_int64(HTTP_PACKAGE(response),"Content-Length",new_cached->content_size_gzip);
			      result = (gchar*)g_memdup(new_cached->content_gzip,new_cached->content_size_gzip);
			      *content_size = new_cached->content_size_gzip;
			    }
			  else if(g_strstr_len(encoding,-1,"deflate"))
			    {
			      http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","deflate",7);
			      http_package_set_int64(HTTP_PACKAGE(response),"Content-Length",new_cached->content_size_deflate);
			      result = (gchar*)g_memdup(new_cached->content_deflate,new_cached->content_size_deflate);
			      *content_size = new_cached->content_size_deflate;
			    }
			  else
			    {
			      http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","identity",8);
			      http_package_set_int64(HTTP_PACKAGE(response),"Content-Length",new_cached->content_size);
			      result = (gchar*)g_memdup(new_cached->content,new_cached->content_size);
			      *content_size = new_cached->content_size;
			    }
			  cache_store = g_list_append(cache_store,new_cached);
			  g_mutex_unlock(&cache_mutex);
			}
		      else
			{
			  const gchar * content_type = websqlite_get_mimetype(real_path);
			  result = file_content;
			  *content_size = file_size;
			  http_package_set_string(HTTP_PACKAGE(response),"Content-Encoding","identity",8);
			  http_package_set_int(HTTP_PACKAGE(response),"Content-Length",file_size);
			  if(content_type)
			    http_package_set_string(HTTP_PACKAGE(response),"Content-Type",content_type,g_utf8_strlen(content_type,256));
			}
		    }
		  else
		    {
		      http_response_set_code(response,HTTP_RESPONSE_INTERNAL_SERVER_ERROR);
		      result = g_strdup_printf("<html><body><h4>ERROR</h4><hr><p>%s</p></body></html>",error->message);
		      *content_size = g_utf8_strlen(result,1024);
		      g_error_free(error);
		      http_package_set_int(HTTP_PACKAGE(response),"Content-Length",g_utf8_strlen(result,1024));
		    }
		}
	      else
		{
		  http_response_set_code(response,HTTP_RESPONSE_NO_CONTENT);
		  *content_size = 0;
		}
	    }
	}
      else
	{
	  http_response_set_code(response,HTTP_RESPONSE_FORBIDDEN);
	  *content_size = 0;
	}
      g_free(real_path);
    }
  return result;
}

WebSQLiteAction *
websqlite_get_action(const gchar * name)
{
  WebSQLiteAction * result = NULL;
  g_mutex_lock(&action_mutex);
  for(GList * iter = g_list_first(action_store);iter;iter = iter->next)
    {
      WebSQLiteAction * action = (WebSQLiteAction * )iter->data;
      if(g_strcmp0(action->name,name) == 0)
	{
	  result = action;
	  break;
	}
    }

  g_mutex_unlock(&action_mutex);
  return result;
}

gchar *
websqlite_action_exec(WebSQLiteAction * action,HttpRequest *request,HttpResponse * response,GSocketConnection * connection,gsize * result_size)
{
  sqlite3_exec(db,"BEGIN TRANSACTION;",NULL,NULL,NULL);
  sqlite3_stmt * result = NULL;
  JsonNode * params = NULL;
  JsonReader * reader = NULL;
  gchar * content = NULL;
  gboolean done = TRUE;

  if(action->method == HTTP_REQUEST_METHOD_POST)
    {
      params = websqlite_parse_params(http_package_get_int(HTTP_PACKAGE(request),"Content-Length"),g_io_stream_get_input_stream(G_IO_STREAM(connection)));
      if(params)
	reader = json_reader_new(params);
    }

  if((action->type == WSQL_ACTION_JSON)||(action->type == WSQL_ACTION_TABLE))
    {
      JsonBuilder * builder = json_builder_new();

      json_builder_begin_object(builder);
      json_builder_set_member_name(builder,"data");
      json_builder_begin_array(builder);
      for(gchar ** statement = action->statements;*statement;statement++)
      	{
	  if(sqlite3_prepare(db,*statement,-1,&result,NULL) == SQLITE_OK)
	    {
	      gint param_count = sqlite3_bind_parameter_count(result);
	      for(gint param_index = 1;param_index <= param_count;param_index ++)
		{
		  const gchar * param_name = sqlite3_bind_parameter_name(result,param_index) + 1;
		  if(g_ascii_strcasecmp(param_name,"INSERTED_ID") == 0)
		    {
		      sqlite3_bind_int64(result,param_index,sqlite3_last_insert_rowid(db));
		    }
		  else if(params)
		    {
		      WebSQLiteParam * param = NULL;
		      guint param_descriptor_count = g_strv_length((gchar**)action->params);
		      for(guint param_descriptor = 0;param_descriptor < param_descriptor_count;param_descriptor++)
			{
			  if(g_ascii_strcasecmp(param_name,action->params[param_descriptor]->name) == 0)
			    {
			      param = action->params[param_descriptor];
			      break;
			    }
			}
		      if(param)
			{
			  if(json_reader_read_member(reader,param->name))
			    {
			      switch(param->type)
			      {
				case WSQL_TYPE_INT:
				  sqlite3_bind_int(result,param_index,json_reader_get_int_value(reader));
				  break;
				case WSQL_TYPE_INT64:
				  sqlite3_bind_int64(result,param_index,json_reader_get_int_value(reader));
				  break;
				case WSQL_TYPE_FLOAT:
				  sqlite3_bind_double(result,param_index,json_reader_get_double_value(reader));
				  break;
				case WSQL_TYPE_TEXT:
				  sqlite3_bind_text(result,param_index,json_reader_get_string_value(reader),-1,NULL);
				  break;
				case WSQL_TYPE_BLOB:
				  {
				    gsize length = 0;
				    gpointer data = g_base64_decode(json_reader_get_string_value(reader),&length);
				    sqlite3_bind_blob(result,param_index,data,length,g_free);
				    break;
				  }
				  break;
			      }
			      json_reader_end_member(reader);
			    }
			  else
			    {
			      sqlite3_bind_null(result,param_index);
			    }
			}
		      else
			{
			  sqlite3_bind_null(result,param_index);
			}
		    }
		  else
		    {
		      sqlite3_bind_null(result,param_index);
		    }
		}

	      if(sqlite3_column_count(result) > 0)
		{
		  if(action->type == WSQL_ACTION_TABLE)
		    {
		      json_builder_begin_object(builder);
		      json_builder_set_member_name(builder,"columns");
		      json_builder_begin_array(builder);
		      gint col_count = sqlite3_column_count(result);
		      for(guint col_index = 0;col_index < col_count;col_index++)
			  json_builder_add_string_value(builder,sqlite3_column_name(result,col_index));
		      json_builder_end_array(builder);

		      json_builder_set_member_name(builder,"rows");
		      json_builder_begin_array(builder);
		      while(sqlite3_step(result) == SQLITE_ROW)
			{
			  json_builder_begin_array(builder);
			  for(gint col_index = 0;col_index < col_count;col_index++)
			    {
			      switch(sqlite3_column_type(result,col_index))
			      {
				case SQLITE_INTEGER:
				  json_builder_add_int_value(builder,sqlite3_column_int64(result,col_index));
				  break;
				case SQLITE_FLOAT:
				  json_builder_add_double_value(builder,sqlite3_column_double(result,col_index));
				  break;
				case SQLITE_TEXT:
				  json_builder_add_string_value(builder,(gchar*)sqlite3_column_text(result,col_index));
				  break;
				case SQLITE_BLOB:
				  {
				    gchar * base64 = g_base64_encode(sqlite3_column_blob(result,col_index),sqlite3_column_bytes(result,col_index));
				    json_builder_add_string_value(builder,base64);
				    g_free(base64);
				  }
				  break;
				case SQLITE_NULL:
				  json_builder_add_null_value(builder);
			      }
			    }
			  json_builder_end_array(builder);
			}
		      json_builder_end_array(builder);
		      json_builder_end_object(builder);
		    }
		  else
		    {
		      json_builder_begin_array(builder);
		      while(sqlite3_step(result) == SQLITE_ROW)
			{
			  gint col_count = sqlite3_column_count(result);
			  json_builder_begin_object(builder);
			  for(gint col_index = 0;col_index < col_count;col_index++)
			    {
			      const gchar * name = sqlite3_column_name(result,col_index);
			      if(name)
				{
				  json_builder_set_member_name(builder,name);
				}
			      else
				{
				  gchar * temp_name = g_strdup_printf("col%d",col_index);
				  json_builder_set_member_name(builder,temp_name);
				  g_free(temp_name);
				}
			      switch(sqlite3_column_type(result,col_index))
			      {
				case SQLITE_INTEGER:
				  json_builder_add_int_value(builder,sqlite3_column_int64(result,col_index));
				  break;
				case SQLITE_FLOAT:
				  json_builder_add_double_value(builder,sqlite3_column_double(result,col_index));
				  break;
				case SQLITE_TEXT:
				  json_builder_add_string_value(builder,(gchar*)sqlite3_column_text(result,col_index));
				  break;
				case SQLITE_BLOB:
				  {
				    gchar * base64 = g_base64_encode(sqlite3_column_blob(result,col_index),sqlite3_column_bytes(result,col_index));
				    json_builder_add_string_value(builder,base64);
				    g_free(base64);
				  }
				  break;
				case SQLITE_NULL:
				  json_builder_add_null_value(builder);
			      }
			    }
			  json_builder_end_object(builder);
			}
		      json_builder_end_array(builder);
		    }
		}
	      else
		{
		  sqlite3_step(result);
		}
	      sqlite3_finalize(result);
	      result = NULL;
	    }
	  else
	    {
	      done = FALSE;
	      break;
	    }
      	}
      json_builder_end_array(builder);
      json_builder_set_member_name(builder,"success");
      json_builder_add_boolean_value(builder,done);
      json_builder_set_member_name(builder,"rows_affected");
      json_builder_add_int_value(builder,sqlite3_changes(db));

      if(done)
	{
	  http_response_set_code(response,HTTP_RESPONSE_OK);
	}
      else
	{
	  http_response_set_code(response,HTTP_RESPONSE_INTERNAL_SERVER_ERROR);
	  json_builder_set_member_name(builder,"exception");
	  json_builder_add_string_value(builder,sqlite3_errmsg(db));
	}
      json_builder_end_object(builder);
      JsonNode * root = json_builder_get_root(builder);
      g_object_unref(builder);
      http_package_set_string(HTTP_PACKAGE(response),"Content-Type","application/json; charset=utf-8",31);
      content = json_to_string(root,FALSE);
      *result_size = strlen(content);
      json_node_unref(root);
      http_package_set_int(HTTP_PACKAGE(response),"Content-Length",*result_size);

    }
  else
    {
      GString * buffer = g_string_new(NULL);
      for(gchar ** statement = action->statements;*statement;statement++)
      	{
	  if(sqlite3_prepare(db,*statement,-1,&result,NULL) == SQLITE_OK)
	    {
	      gint param_count = sqlite3_bind_parameter_count(result);
	      for(gint param_index = 1;param_index <= param_count;param_index ++)
		{
		  const gchar * param_name = sqlite3_bind_parameter_name(result,param_index) + 1;
		  if(g_ascii_strcasecmp(param_name,"INSERTED_ID") == 0)
		    {
		      sqlite3_bind_int64(result,param_index,sqlite3_last_insert_rowid(db));
		    }
		  else if(params)
		    {
		      WebSQLiteParam * param = NULL;
		      guint param_descriptor_count = g_strv_length((gchar**)action->params);
		      for(guint param_descriptor = 0;param_descriptor < param_descriptor_count;param_descriptor++)
			{
			  if(g_ascii_strcasecmp(param_name,action->params[param_descriptor]->name) == 0)
			    {
			      param = action->params[param_descriptor];
			      break;
			    }
			}
		      if(param)
			{
			  if(json_reader_read_member(reader,param->name))
			    {
			      switch(param->type)
			      {
				case WSQL_TYPE_INT:
				  sqlite3_bind_int(result,param_index,json_reader_get_int_value(reader));
				  break;
				case WSQL_TYPE_INT64:
				  sqlite3_bind_int64(result,param_index,json_reader_get_int_value(reader));
				  break;
				case WSQL_TYPE_FLOAT:
				  sqlite3_bind_double(result,param_index,json_reader_get_double_value(reader));
				  break;
				case WSQL_TYPE_TEXT:
				  sqlite3_bind_text(result,param_index,json_reader_get_string_value(reader),-1,NULL);
				  break;
				case WSQL_TYPE_BLOB:
				  {
				    gsize length = 0;
				    gpointer data = g_base64_decode(json_reader_get_string_value(reader),&length);
				    sqlite3_bind_blob(result,param_index,data,length,g_free);
				    break;
				  }
				  break;
			      }
			      json_reader_end_member(reader);
			    }
			  else
			    {
			      sqlite3_bind_null(result,param_index);
			    }
			}
		      else
			{
			  sqlite3_bind_null(result,param_index);
			}
		    }
		  else
		    {
		      sqlite3_bind_null(result,param_index);
		    }
		}
	      while(sqlite3_step(result) == SQLITE_ROW)
		{
		  gint col_count = sqlite3_column_count(result);
		  for(gint col_index = 0;col_index < col_count;col_index++)
		    {
		      if(sqlite3_column_text(result,col_index))
			g_string_append(buffer,(gchar*)sqlite3_column_text(result,col_index));
		    }

		}
	      sqlite3_finalize(result);
	      result = NULL;
	    }
	  else
	    {
	      done = FALSE;
	    }
      	}
      if(done)
	{
	  http_response_set_code(response,HTTP_RESPONSE_OK);
	  if(action->mimetype)
	    http_package_set_string(HTTP_PACKAGE(response),"Content-Type",action->mimetype,g_utf8_strlen(action->mimetype,100));
	  http_package_set_int(HTTP_PACKAGE(response),"Content-Length",buffer->len);
	}
      *result_size = buffer->len;
      content = g_string_free(buffer,FALSE);
    }

  if(params)
    {
      json_node_unref(params);
      g_object_unref(reader);
    }

  if(done)
    sqlite3_exec(db,"COMMIT TRANSACTION;",NULL,NULL,NULL);
  else
    sqlite3_exec(db,"ROLLBACK TRANSACTION;",NULL,NULL,NULL);

  return content;
}

const gchar *
websqlite_get_mimetype(const gchar * filename)
{
  for(GList * iter = g_list_first(type_store);iter;iter = g_list_next(iter))
    {
      WebSQLiteMimeType * mimetype = (WebSQLiteMimeType *)iter->data;
      if(g_str_has_suffix(filename,mimetype->ext))
	return mimetype->mimetype;
    }
  return NULL;
}

gchar **
websqlite_split_statements(const gchar * str)
{
  GList * statements = NULL;
  gboolean in_literal = FALSE;
  const gchar * last_cut = str;
  for(const gchar * iter = str;*iter;iter ++)
    {
      if(!in_literal)
	{
	  if(*iter == '\'')
	    in_literal = FALSE;
	}
      else if(*iter == '\'')
	{
	  in_literal = TRUE;
	}
      if(*iter == ';' && !in_literal)
	{
	  gchar * statement = g_strndup(last_cut,(iter - last_cut) + 1);
	  g_strstrip(statement);
	  if(g_utf8_strlen(statement,4048))
	    {
	      statements = g_list_append(statements,statement);
	    }
	  else
	    {
	      g_free(statement);
	    }
	  last_cut = iter + 1;
	}
    }
  if(last_cut < str)
    {
      gchar * statement = g_strdup(last_cut);
      if(g_utf8_strlen(statement,4048))
	{
	  statements = g_list_append(statements,statement);
	}
      else
	{
	  g_free(statement);
	}
    }
  gchar ** result = g_new0(gchar*,g_list_length(statements) + 1);
  guint list_index = 0;
  for(GList * iter = g_list_first(statements);iter;iter = g_list_next(iter))
    {
      result[list_index] = (gchar*)iter->data;
      list_index++;
    }
  g_list_free(g_list_first(statements));
  return result;
}

void
websqlite_parse(const gchar * action_base,const gchar *  filename)
{
  FILE * wsql_file = fopen(filename,"r");
  gchar * statement = NULL;
  gsize	  statement_size = 0;

  WebSQLiteAction * action = NULL;
  GList * params = NULL;
  gboolean is_body = FALSE;
  GString * body = NULL;

  while(!feof(wsql_file))
    {
      if(getline(&statement,&statement_size,wsql_file)!= -1)
	{
	  gchar * statement_original = g_strdup(statement);
	  gchar action_type[100];
	  g_strstrip(statement);

	  if(g_strcmp0(statement,"") != 0)
	    {
	      sscanf(statement,"%s",action_type);
	      if(!action)
		{

		  if((g_ascii_strcasecmp(action_type,"post") == 0) || (g_ascii_strcasecmp(action_type,"get") == 0))
		    {
		      gchar action_name[256] = {0,};
		      gchar action_result_type[100] = {0,};
		      memset(action_name,0,256);
		      memset(action_result_type,0,100);
		      sscanf(statement + g_utf8_strlen(action_type,10),"%s %s",action_name,action_result_type);
		      action = g_new0(WebSQLiteAction,1);
		      action->method = g_ascii_strcasecmp(action_type,"post") == 0 ?
			  HTTP_REQUEST_METHOD_POST :
			  HTTP_REQUEST_METHOD_GET;
		      action->name = g_strdup_printf("/%s/%s",action_base,action_name);
		      if(g_ascii_strcasecmp(action_result_type,"json") == 0)
			action->type = WSQL_ACTION_JSON;
		      else if (g_ascii_strcasecmp(action_result_type,"table") == 0)
			action->type = WSQL_ACTION_TABLE;
		      else
			action->type = WSQL_ACTION_TEXT;
		    }
		  else
		    {
		      //TODO: error

		    }
		}
	      else
		{
		  if(is_body)
		    {
		      if(g_ascii_strcasecmp(action_type,"end") == 0)
			{
			  gchar * str_body = g_string_free(body,FALSE);
			  action->statements = websqlite_split_statements(str_body);
			  g_free(str_body);
			  action->params = g_new0(WebSQLiteParam*,g_list_length(params) + 1);
			  guint param_index = 0;
			  for(GList * iter = g_list_first(params);iter;iter = iter->next)
			    {
			      action->params[param_index] = (WebSQLiteParam*)iter->data;
			      param_index++;
			    }
			  g_list_free(g_list_first(params));

			  params = NULL;
			  is_body = FALSE;
			  action_store = g_list_append(action_store,action);
			  action = NULL;
			}
		      else
			{
			  g_string_append(body,statement_original);
			}
		    }
		  else
		    {
		      WebSQLiteParam * param = NULL;
		      if(g_ascii_strcasecmp(action_type,"mimetype") == 0)
			{
			  action->mimetype = g_strdup(statement + 8);
			  g_strstrip(action->mimetype);
			}
		      else if(g_ascii_strcasecmp(action_type,"int") == 0)
			{
			  param = g_new0(WebSQLiteParam,1);
			  param->name = g_strdup(statement + 3);
			  g_strstrip(param->name);
			  param->type = WSQL_TYPE_INT;
			  params = g_list_append(params,param);
			}
		      else if(g_ascii_strcasecmp(action_type,"text") == 0)
			{
			  param = g_new0(WebSQLiteParam,1);
			  param->name = g_strdup(statement + 4);
			  g_strstrip(param->name);
			  param->type = WSQL_TYPE_TEXT;
			  params = g_list_append(params,param);
			}
		      else if(g_ascii_strcasecmp(action_type,"int64") == 0)
			{
			  param = g_new0(WebSQLiteParam,1);
			  param->name = g_strdup(statement + 5);
			  g_strstrip(param->name);
			  param->type = WSQL_TYPE_INT64;
			  params = g_list_append(params,param);
			}
		      else if(g_ascii_strcasecmp(action_type,"blob") == 0)
			{
			  param = g_new0(WebSQLiteParam,1);
			  param->name = g_strdup(statement + 4);
			  g_strstrip(param->name);
			  param->type = WSQL_TYPE_BLOB;
			  params = g_list_append(params,param);
			}
		      else if(g_ascii_strcasecmp(action_type,"float") == 0)
			{
			  param = g_new0(WebSQLiteParam,1);
			  param->name = g_strdup(statement + 4);
			  g_strstrip(param->name);
			  param->type = WSQL_TYPE_FLOAT;
			  params = g_list_append(params,param);
			}
		      else if(g_ascii_strcasecmp(action_type,"as") == 0)
			{
			  is_body = TRUE;
			  body = g_string_new(NULL);
			}
		      else
			{
			  //TODO:error
			}
		    }
		}
	    }
	  g_free(statement_original);
	}
      else
	{
	  break;
	}
    }
  fclose(wsql_file);
  if(statement)
    g_free(statement);
}
