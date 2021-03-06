/*
	This file is part of qt5-ora-plugin.

	qt5-ora-plugin is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	qt5-ora-plugin is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with qt5-ora-plugin.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "OraHandler.hpp"

#include <cstring>

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>

#include <archive_entry.h>

using namespace pugi;

bool OraHandler::canRead() const{
	//TODO: determine peek size
	return device()->peek( 128 ).contains( "image/openraster" );
}

static QByteArray read_data( archive* a ){
	QByteArray raw;
	
	//Read chuncks
	const char *buff;
	size_t size;
	int64_t offset;
	
	while( true ){
		switch( archive_read_data_block( a, (const void**)&buff, &size, &offset ) ){
			case ARCHIVE_OK: raw += QByteArray( buff, size ); break;
			case ARCHIVE_EOF: return raw;
			default:
				qWarning( "Error while reading zip data: %s", archive_error_string(a) );
				return raw;
		}
	}
}

static QImage read_image( archive* a, const char* format=nullptr ){
	QByteArray raw = read_data( a );
	QBuffer buf( &raw );
	QImageReader reader( (QIODevice*)&buf, format );
	return reader.canRead() ? reader.read() : QImage();
}

static QString next_file( archive* a ){
	archive_entry *entry;
	switch( archive_read_next_header( a, &entry ) ){
		case ARCHIVE_EOF: return QString();
		case ARCHIVE_OK:
			return QString::fromWCharArray( archive_entry_pathname_w( entry ) );
			
		default:
			qWarning( "Can't read the next zip header: %s", archive_error_string(a) );
			return QString();
	}
}

bool OraHandler::read_and_validate( archive *a ){
	//Read and check mime
	QString mime = next_file( a );
	if( mime.isNull() ){
		qWarning( "Ora image is not an usable zip archive" );
		return false;
	}
		
	if( mime != "mimetype" ){
		qWarning( "Ora image must start with the mimetype" );
		return false;
	}
	//TODO: check if it is STORED?
	
	QByteArray mime_data = read_data( a );
	if( mime_data != "image/openraster" ){
		qWarning( "Mimetype does not match" );
		return false;
	}
	
	//Read all files
	QString name;
	while( !(name = next_file( a )).isNull() ){
		if( name.startsWith( "data/" ) ){
			QString suffix = QFileInfo(name).suffix();
			QImage img = read_image( a, suffix.toLocal8Bit().constData() );
			if( !img.isNull() )
				images.insert( std::pair<QString,QImage>( name, img ) );
			else
				qWarning( "Could not read data file: %s", name.toLocal8Bit().constData() );
		}
		else if( name == "Thumbnails/thumbnail.png" ){
			thumbnail = read_image( a );
		}
		else if( name == "mergedimage.png" ){
			merged = read_image( a );
			if( merged.isNull() )
				qWarning( "Merged image could not be loaded" );
		}
		else if( name == "stack.xml" ){
			QByteArray data = read_data( a );
			if( !doc.load_buffer( data.data(), data.size() ) )
				return false;
		}
		else
			qWarning( "Unknown file: %s", name.toLocal8Bit().constData() );
	}
	
	if( thumbnail.isNull() ){
		qWarning( "Ora image does not contain thumbnail as required" );
		return false;
	}
	//TODO: check for stack.xml
	
	return true;
}

struct ReadingData{
	QIODevice& device;
	qint64 buff_size; //TODO: how large should this be?
	QByteArray arr;
	
	ReadingData( QIODevice& device, int buff_size = 1024*4 ) : device( device ), buff_size(buff_size) { }
};
ssize_t stream_read( archive*, void* data, const void** buff ){
	ReadingData& stream = *(ReadingData*)data;
	stream.arr = stream.device.read( stream.buff_size );
	*buff = stream.arr.constData();
	return stream.arr.size();
}

int stream_close( archive*, void *data ){
	((ReadingData*)data)->device.close();
	return ARCHIVE_OK;
}

bool OraHandler::load(){
	bool success = true;
	archive* a = archive_read_new();
	archive_read_support_format_zip(a);
	
	ReadingData data( *device() );
	if( archive_read_open( a, &data, nullptr, stream_read, stream_close ) ){
		qDebug( "couldn't open: %s", archive_error_string(a) );
		success = false;
	}
	else
		success = read_and_validate( a );
	
	archive_read_close( a );
	archive_read_free( a );
	
	return success;
}


bool ora_composite_mode( std::string text, QPainter::CompositionMode &mode ){
	static std::map<std::string,QPainter::CompositionMode> translator{
			{ "", QPainter::CompositionMode_SourceOver }
		,	{ "svg:src-over", QPainter::CompositionMode_SourceOver }
		,	{ "svg:plus", QPainter::CompositionMode_Plus }
		,	{ "svg:multiply", QPainter::CompositionMode_Multiply }
		,	{ "svg:screen", QPainter::CompositionMode_Screen }
		,	{ "svg:overlay", QPainter::CompositionMode_Overlay }
		,	{ "svg:darken", QPainter::CompositionMode_Darken }
		,	{ "svg:lighten", QPainter::CompositionMode_Lighten }
		,	{ "svg:color-dodge", QPainter::CompositionMode_ColorDodge }
		,	{ "svg:color-burn", QPainter::CompositionMode_ColorBurn }
		,	{ "svg:hard-light", QPainter::CompositionMode_HardLight }
		,	{ "svg:soft-light", QPainter::CompositionMode_SoftLight }
		,	{ "svg:difference", QPainter::CompositionMode_Difference }
		
		/* Doesn't appear to be supported by QPainter
		,	{ "svg:color", QPainter::CompositionMode_SourceOver }
		,	{ "svg:luminosity", QPainter::CompositionMode_SourceOver }
		,	{ "svg:hue", QPainter::CompositionMode_SourceOver }
		,	{ "svg:saturation", QPainter::CompositionMode_SourceOver }
		*/
		};
	
	auto it = translator.find( text );
	if( it != translator.end() ){
		mode = (*it).second;
		return true;
	}
	return false;
}

void OraHandler::render_stack( xml_node node, QPainter &painter, int offset_x, int offset_y ) const{
	for( xml_node_iterator it = --node.end(); it != --node.begin(); it-- ){
		std::string name( (*it).name() );
		if( name == "stack" ){
			int x = (*it).attribute( "x" ).as_int( 0 );
			int y = (*it).attribute( "y" ).as_int( 0 );
			
			render_stack( *it, painter, offset_x+x, offset_y+y );
		}
		else if( name == "text" ){
			qWarning( "No support for text" );
		}
		else if( name == "layer" ){
			QString source( QString::fromUtf8( (*it).attribute( "src" ).value() ) );
			int x = (*it).attribute( "x" ).as_int( 0 ) + offset_x;
			int y = (*it).attribute( "y" ).as_int( 0 ) + offset_y;
			
			std::string visibility = (*it).attribute( "visibility" ).value();
			if( visibility == "" || visibility == "visible" ){
				//composite-op
				std::string composite = (*it).attribute( "composite-op" ).value();
				QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
				if( !ora_composite_mode( composite, mode ) )
					qWarning( "Unsupported composite-op: %s", composite.c_str() );
				painter.setCompositionMode( mode );
				
				double opacity = (*it).attribute( "opacity" ).as_double( 1.0 );
				painter.setOpacity( opacity );
				
				std::map<QString,QImage>::const_iterator img_it = images.find( source );
				if( img_it != images.end() )
					painter.drawImage( x, y, img_it->second );
				else
					qWarning( "Layer source not found: %s", source.toLocal8Bit().constData() );
				
				//Restore modified settings
				painter.setOpacity( 1.0 );
				painter.setCompositionMode( QPainter::CompositionMode_SourceOver );
			}
		}
		else{
			qWarning( "Unrecognized element: %s", name.c_str() );
		}
	}
}

bool OraHandler::read( QImage *img_pointer ){
	frame++;
	*img_pointer = QImage();
	
	if( frame == 1 )
		if( !load() )
			return false;
	
	if( frame > 1 )
		return false;
	
	if( !merged.isNull() )
		*img_pointer = merged;
	else{
		xml_node img = doc.child( "image" );
		int width = img.attribute( "w" ).as_int( -1 );
		int height = img.attribute( "h" ).as_int( -1 );
		if( width <= 0 || height <= 0 ){
			qWarning( "Image dimensions are invalid" );
			return false;
		}
		
		QImage output( width, height, QImage::Format_ARGB32_Premultiplied );
		output.fill( qRgba( 0,0,0,0 ) );
		QPainter painter( &output );
		render_stack( img, painter );
		*img_pointer = output;
	}
	
	return true;
}


