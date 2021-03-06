/***************************************************************************
    qgsogrfeatureiterator.cpp
    ---------------------
    begin                : Juli 2012
    copyright            : (C) 2012 by Martin Dobias
    email                : wonder dot sk at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgsogrfeatureiterator.h"

#include "qgsogrprovider.h"
#include "qgsogrexpressioncompiler.h"
#include "qgssqliteexpressioncompiler.h"

#include "qgsogrutils.h"
#include "qgsapplication.h"
#include "qgsgeometry.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgssettings.h"

#include <QTextCodec>
#include <QFile>

// using from provider:
// - setRelevantFields(), mRelevantFieldsForNextFeature
// - ogrLayer
// - mFetchFeaturesWithoutGeom
// - mAttributeFields
// - mEncoding


QgsOgrFeatureIterator::QgsOgrFeatureIterator( QgsOgrFeatureSource *source, bool ownSource, const QgsFeatureRequest &request )
  : QgsAbstractFeatureIteratorFromSource<QgsOgrFeatureSource>( source, ownSource, request )
  , mFeatureFetched( false )
  , mConn( nullptr )
  , ogrLayer( nullptr )
  , mSubsetStringSet( false )
  , mFetchGeometry( false )
  , mExpressionCompiled( false )
  , mFilterFids( mRequest.filterFids() )
  , mFilterFidsIt( mFilterFids.constBegin() )
{
  mConn = QgsOgrConnPool::instance()->acquireConnection( mSource->mProvider->dataSourceUri() );
  if ( !mConn->ds )
  {
    return;
  }

  if ( mSource->mLayerName.isNull() )
  {
    ogrLayer = OGR_DS_GetLayer( mConn->ds, mSource->mLayerIndex );
  }
  else
  {
    ogrLayer = OGR_DS_GetLayerByName( mConn->ds, mSource->mLayerName.toUtf8().constData() );
  }
  if ( !ogrLayer )
  {
    return;
  }

  if ( !mSource->mSubsetString.isEmpty() )
  {
    ogrLayer = QgsOgrProviderUtils::setSubsetString( ogrLayer, mConn->ds, mSource->mEncoding, mSource->mSubsetString );
    if ( !ogrLayer )
    {
      return;
    }
    mSubsetStringSet = true;
  }

  mFetchGeometry = ( !mRequest.filterRect().isNull() ) || !( mRequest.flags() & QgsFeatureRequest::NoGeometry );
  QgsAttributeList attrs = ( mRequest.flags() & QgsFeatureRequest::SubsetOfAttributes ) ? mRequest.subsetOfAttributes() : mSource->mFields.allAttributesList();

  // ensure that all attributes required for expression filter are being fetched
  if ( mRequest.flags() & QgsFeatureRequest::SubsetOfAttributes && request.filterType() == QgsFeatureRequest::FilterExpression )
  {
    //ensure that all fields required for filter expressions are prepared
    QSet<int> attributeIndexes = request.filterExpression()->referencedAttributeIndexes( mSource->mFields );
    attributeIndexes += attrs.toSet();
    attrs = attributeIndexes.toList();
    mRequest.setSubsetOfAttributes( attrs );
  }
  if ( request.filterType() == QgsFeatureRequest::FilterExpression && request.filterExpression()->needsGeometry() )
  {
    mFetchGeometry = true;
  }

  // make sure we fetch just relevant fields
  // unless it's a VRT data source filtered by geometry as we don't know which
  // attributes make up the geometry and OGR won't fetch them to evaluate the
  // filter if we choose to ignore them (fixes #11223)
  if ( ( mSource->mDriverName != QLatin1String( "VRT" ) && mSource->mDriverName != QLatin1String( "OGR_VRT" ) ) || mRequest.filterRect().isNull() )
  {
    QgsOgrProviderUtils::setRelevantFields( ogrLayer, mSource->mFields.count(), mFetchGeometry, attrs, mSource->mFirstFieldIsFid );
  }

  // spatial query to select features
  if ( !mRequest.filterRect().isNull() )
  {
    const QgsRectangle &rect = mRequest.filterRect();

    OGR_L_SetSpatialFilterRect( ogrLayer, rect.xMinimum(), rect.yMinimum(), rect.xMaximum(), rect.yMaximum() );
  }
  else
  {
    OGR_L_SetSpatialFilter( ogrLayer, nullptr );
  }

  if ( request.filterType() == QgsFeatureRequest::FilterExpression
       && QgsSettings().value( QStringLiteral( "qgis/compileExpressions" ), true ).toBool() )
  {
    QgsSqlExpressionCompiler *compiler = nullptr;
    if ( source->mDriverName == QLatin1String( "SQLite" ) || source->mDriverName == QLatin1String( "GPKG" ) )
    {
      compiler = new QgsSQLiteExpressionCompiler( source->mFields );
    }
    else
    {
      compiler = new QgsOgrExpressionCompiler( source );
    }

    QgsSqlExpressionCompiler::Result result = compiler->compile( request.filterExpression() );
    if ( result == QgsSqlExpressionCompiler::Complete || result == QgsSqlExpressionCompiler::Partial )
    {
      QString whereClause = compiler->result();
      if ( OGR_L_SetAttributeFilter( ogrLayer, mSource->mEncoding->fromUnicode( whereClause ).constData() ) == OGRERR_NONE )
      {
        //if only partial success when compiling expression, we need to double-check results using QGIS' expressions
        mExpressionCompiled = ( result == QgsSqlExpressionCompiler::Complete );
        mCompileStatus = ( mExpressionCompiled ? Compiled : PartiallyCompiled );
      }
    }
    else
    {
      OGR_L_SetAttributeFilter( ogrLayer, nullptr );
    }

    delete compiler;
  }
  else
  {
    OGR_L_SetAttributeFilter( ogrLayer, nullptr );
  }

  //start with first feature
  rewind();
}

QgsOgrFeatureIterator::~QgsOgrFeatureIterator()
{
  close();
}

bool QgsOgrFeatureIterator::nextFeatureFilterExpression( QgsFeature &f )
{
  if ( !mExpressionCompiled )
    return QgsAbstractFeatureIterator::nextFeatureFilterExpression( f );
  else
    return fetchFeature( f );
}

bool QgsOgrFeatureIterator::fetchFeatureWithId( QgsFeatureId id, QgsFeature &feature ) const
{
  feature.setValid( false );
  OGRFeatureH fet = OGR_L_GetFeature( ogrLayer, FID_TO_NUMBER( id ) );
  if ( !fet )
  {
    return false;
  }

  if ( readFeature( fet, feature ) )
    OGR_F_Destroy( fet );

  feature.setValid( true );
  return true;
}

bool QgsOgrFeatureIterator::fetchFeature( QgsFeature &feature )
{
  feature.setValid( false );

  if ( mClosed || !ogrLayer )
    return false;

  if ( mRequest.filterType() == QgsFeatureRequest::FilterFid )
  {
    bool result = fetchFeatureWithId( mRequest.filterFid(), feature );
    close(); // the feature has been read or was not found: we have finished here
    return result;
  }
  else if ( mRequest.filterType() == QgsFeatureRequest::FilterFids )
  {
    while ( mFilterFidsIt != mFilterFids.constEnd() )
    {
      QgsFeatureId nextId = *mFilterFidsIt;
      mFilterFidsIt++;

      if ( fetchFeatureWithId( nextId, feature ) )
        return true;
    }
    close();
    return false;
  }

  OGRFeatureH fet;

  while ( ( fet = OGR_L_GetNextFeature( ogrLayer ) ) )
  {
    if ( !readFeature( fet, feature ) )
      continue;
    else
      OGR_F_Destroy( fet );

    if ( !mRequest.filterRect().isNull() && !feature.hasGeometry() )
      continue;

    // we have a feature, end this cycle
    feature.setValid( true );
    return true;

  } // while

  close();
  return false;
}


bool QgsOgrFeatureIterator::rewind()
{
  if ( mClosed || !ogrLayer )
    return false;

  OGR_L_ResetReading( ogrLayer );

  mFilterFidsIt = mFilterFids.constBegin();

  return true;
}


bool QgsOgrFeatureIterator::close()
{
  if ( !mConn )
    return false;

  iteratorClosed();

  // Will for example release SQLite3 statements
  if ( ogrLayer )
  {
    OGR_L_ResetReading( ogrLayer );
  }

  if ( mSubsetStringSet )
  {
    OGR_DS_ReleaseResultSet( mConn->ds, ogrLayer );
  }

  if ( mConn )
    QgsOgrConnPool::instance()->releaseConnection( mConn );

  mConn = nullptr;
  ogrLayer = nullptr;

  mClosed = true;
  return true;
}


void QgsOgrFeatureIterator::getFeatureAttribute( OGRFeatureH ogrFet, QgsFeature &f, int attindex ) const
{
  if ( mSource->mFirstFieldIsFid && attindex == 0 )
  {
    f.setAttribute( 0, static_cast<qint64>( OGR_F_GetFID( ogrFet ) ) );
    return;
  }

  int attindexWithoutFid = ( mSource->mFirstFieldIsFid ) ? attindex - 1 : attindex;
  bool ok = false;
  QVariant value = QgsOgrUtils::getOgrFeatureAttribute( ogrFet, mSource->mFieldsWithoutFid, attindexWithoutFid, mSource->mEncoding, &ok );
  if ( !ok )
    return;

  f.setAttribute( attindex, value );
}


bool QgsOgrFeatureIterator::readFeature( OGRFeatureH fet, QgsFeature &feature ) const
{
  feature.setId( OGR_F_GetFID( fet ) );
  feature.initAttributes( mSource->mFields.count() );
  feature.setFields( mSource->mFields ); // allow name-based attribute lookups

  bool useIntersect = mRequest.flags() & QgsFeatureRequest::ExactIntersect;
  bool geometryTypeFilter = mSource->mOgrGeometryTypeFilter != wkbUnknown;
  if ( mFetchGeometry || useIntersect || geometryTypeFilter )
  {
    OGRGeometryH geom = OGR_F_GetGeometryRef( fet );

    if ( geom )
    {
      feature.setGeometry( QgsOgrUtils::ogrGeometryToQgsGeometry( geom ) );
    }
    else
      feature.clearGeometry();

    if ( mSource->mOgrGeometryTypeFilter == wkbGeometryCollection &&
         geom && wkbFlatten( OGR_G_GetGeometryType( geom ) ) == wkbGeometryCollection )
    {
      // OK
    }
    else if ( ( useIntersect && ( !feature.hasGeometry() || !feature.geometry().intersects( mRequest.filterRect() ) ) )
              || ( geometryTypeFilter && ( !feature.hasGeometry() || QgsOgrProvider::ogrWkbSingleFlatten( ( OGRwkbGeometryType )feature.geometry().wkbType() ) != mSource->mOgrGeometryTypeFilter ) ) )
    {
      OGR_F_Destroy( fet );
      return false;
    }
  }

  if ( !mFetchGeometry )
  {
    feature.clearGeometry();
  }

  // fetch attributes
  if ( mRequest.flags() & QgsFeatureRequest::SubsetOfAttributes )
  {
    QgsAttributeList attrs = mRequest.subsetOfAttributes();
    for ( QgsAttributeList::const_iterator it = attrs.begin(); it != attrs.end(); ++it )
    {
      getFeatureAttribute( fet, feature, *it );
    }
  }
  else
  {
    // all attributes
    for ( int idx = 0; idx < mSource->mFields.count(); ++idx )
    {
      getFeatureAttribute( fet, feature, idx );
    }
  }

  return true;
}


QgsOgrFeatureSource::QgsOgrFeatureSource( const QgsOgrProvider *p )
  : mProvider( p )
{
  mDataSource = p->dataSourceUri();
  mLayerName = p->layerName();
  mLayerIndex = p->layerIndex();
  mSubsetString = p->mSubsetString;
  mEncoding = p->textEncoding(); // no copying - this is a borrowed pointer from Qt
  mFields = p->mAttributeFields;
  for ( int i = ( p->mFirstFieldIsFid ) ? 1 : 0; i < mFields.size(); i++ )
    mFieldsWithoutFid.append( mFields.at( i ) );
  mDriverName = p->ogrDriverName;
  mFirstFieldIsFid = p->mFirstFieldIsFid;
  mOgrGeometryTypeFilter = QgsOgrProvider::ogrWkbSingleFlatten( p->mOgrGeometryTypeFilter );
  QgsOgrConnPool::instance()->ref( mDataSource );
}

QgsOgrFeatureSource::~QgsOgrFeatureSource()
{
  QgsOgrConnPool::instance()->unref( mDataSource );
}

QgsFeatureIterator QgsOgrFeatureSource::getFeatures( const QgsFeatureRequest &request )
{
  return QgsFeatureIterator( new QgsOgrFeatureIterator( this, false, request ) );
}
