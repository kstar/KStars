/***************************************************************************
                          starcomponent.cpp  -  K Desktop Planetarium
                             -------------------
    begin                : 2005/14/08
    copyright            : (C) 2005 by Thomas Kabelmann
    email                : thomas.kabelmann@gmx.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "starcomponent.h"

#include <QPixmap>
#include <QPainter>

#include <QRectF>
#include <QFontMetricsF>
#include <kglobal.h>

#include "Options.h"
#include "kstarsdata.h"
#include "ksutils.h"
#include "skymap.h"
#include "starobject.h"

#include "skymesh.h"
#include "skylabel.h"
#include "skylabeler.h"
#include "kstarssplash.h"

#include "binfilehelper.h"
#include "byteswap.h"
#include "starblockfactory.h"

#include <kde_file.h>

StarComponent::StarComponent(SkyComponent *parent )
    : ListComponent(parent), m_reindexNum(J2000), m_FaintMagnitude(-5.0), starsLoaded(false)
{
    m_skyMesh = SkyMesh::Instance();
    m_StarBlockFactory = StarBlockFactory::Instance();

    m_starIndex = new StarIndex();
    for (int i = 0; i < m_skyMesh->size(); i++)
        m_starIndex->append( new StarList() );
    m_highPMStars.append( new HighPMStarList( 840.0 ) );
    m_highPMStars.append( new HighPMStarList( 304.0 ) );
    m_reindexInterval = StarObject::reindexInterval( 304.0 );

    m_zoomMagLimit = 0.0;
    m_reloadSplash = m_reindexSplash = 0;
    m_validLineNums = false;

    for ( int i = 0; i <= MAX_LINENUMBER_MAG; i++ )
        m_labelList[ i ] = new LabelList;
}
StarComponent::~StarComponent() {
    // Empty
}

bool StarComponent::selected() {
    return Options::showStars();
}

void StarComponent::init(KStarsData *data) {
    emitProgressText( i18n("Loading stars" ) );
    m_Data = data;

    loadStaticData();

    // Load any deep star catalogs that are available
    loadDeepStarCatalogs();

    StarObject::initImages();
}

bool StarComponent::addDeepStarCatalogIfExists( const QString &fileName, float trigMag, bool staticstars ) {
    if( BinFileHelper::testFileExists( fileName ) ) {
        DeepStarComponent *newdsc;
        m_DeepStarComponents.append( newdsc = new DeepStarComponent( this, fileName, trigMag, staticstars ) );
        newdsc->init( KStarsData::Instance() );
        return true;
    }
    else
        return false;
}


int StarComponent::loadDeepStarCatalogs() {
    
    // Look for the basic unnamed star catalog to mag 8.0
    if( !addDeepStarCatalogIfExists( "unnamedstars.dat", -5.0, true ) )
        return 0;

    // Look for the Tycho-2 add-on with 2.5 million stars to mag 12.5
    if( !addDeepStarCatalogIfExists( "tycho2.dat" , 8.0 ) && !addDeepStarCatalogIfExists( "deepstars.dat", 8.0 ) )
        return 1;

    // Look for the USNO NOMAD 1e8 star catalog add-on with stars to mag 16
    if( !addDeepStarCatalogIfExists( "USNO-NOMAD-1e8.dat", 11.0 ) )
        return 2;

    return 3;
}

//This function is empty for a reason; we override the normal 
//update function in favor of JiT updates for stars.
void StarComponent::update( KStarsData *data, KSNumbers *num )   
{   
    Q_UNUSED(data)   
    Q_UNUSED(num)   
}   

// We use the update hook to re-index all the stars when the date has changed by
// more than 150 years.

void StarComponent::reindex( KSNumbers *num )
{
    if ( ! num ) return;

    // for large time steps we re-index all points
    if ( fabs( num->julianCenturies() -
               m_reindexNum.julianCenturies() ) > m_reindexInterval ) {
        reindexAll( num );
        return;
    }

    // otherwise we just re-index fast movers as needed
    for ( int j = 0; j < m_highPMStars.size(); j++ )
        m_highPMStars.at( j )->reindex( num, m_starIndex );
}

void StarComponent::reindexAll( KSNumbers *num )
{
    if (  0 && ! m_reindexSplash ) {
        m_reindexSplash = new KStarsSplash(0,
                                           i18n("Please wait while re-indexing stars ...") );
        QObject::connect( KStarsData::Instance(),
                          SIGNAL( progressText( QString ) ),
                          m_reindexSplash, SLOT( setMessage( QString ) ) );

        m_reindexSplash->show();
        m_reindexSplash->raise();
        return;
    }

    printf("Re-indexing Stars to year %4.1f...\n",
           2000.0 + num->julianCenturies() * 100.0);

    m_reindexNum = KSNumbers( *num );
    m_skyMesh->setKSNumbers( num );

    // clear out the old index
    for ( int i = 0; i < m_starIndex->size(); i++ ) {
        m_starIndex->at( i )->clear();
    }

    // re-populate it from the objectList
    int size = objectList().size();
    for ( int i = 0; i < size; i++ ) {
        StarObject* star = (StarObject*) objectList()[ i ];
        Trixel trixel = m_skyMesh->indexStar( star );
        m_starIndex->at( trixel )->append( star );
    }

    // Let everyone else know we have re-indexed to num
    for ( int j = 0; j < m_highPMStars.size(); j++ ) {
        m_highPMStars.at( j )->setIndexTime( num );
    }

    //delete m_reindexSplash;
    //m_reindexSplash = 0;

    printf("Done.\n");
}



void StarComponent::draw( QPainter& psky )
{
    if ( ! selected() ) return;

    SkyMap *map = SkyMap::Instance();
    KStarsData* data = KStarsData::Instance();
    UpdateID updateID = data->updateID();

    bool checkSlewing = ( map->isSlewing() && Options::hideOnSlew() );
    m_hideLabels =  ( map->isSlewing() && Options::hideLabels() ) ||
                    ! ( Options::showStarMagnitudes() || Options::showStarNames() );

    //shortcuts to inform whether to draw different objects
    bool hideFaintStars( checkSlewing && Options::hideStars() );
    double hideStarsMag = Options::magLimitHideStar();

    reindex( data->updateNum() );

    //adjust maglimit for ZoomLevel
    double lgmin = log10(MINZOOM);
    double lgmax = log10(MAXZOOM);
    double lgz = log10(Options::zoomFactor());

    // Old formula:
    //    float maglim = ( 2.000 + 2.444 * Options::memUsage() / 10.0 ) * ( lgz - lgmin ) + Options::magLimitDrawStarZoomOut();

    /*
     Explanation for the following formula:
     --------------------------------------
     Estimates from a sample of 125000 stars shows that, magnitude 
     limit vs. number of stars follows the formula:
       nStars = 10^(.45 * maglim + .95)
     (A better formula is available here: http://www.astro.uu.nl/~strous/AA/en/antwoorden/magnituden.html
      which we do not implement for simplicity)
     We want to keep the star density on screen a constant. This is directly proportional to the number of stars
     and directly proportional to the area on screen. The area is in turn inversely proportional to the square
     of the zoom factor ( zoomFactor / MINZOOM ). This means that (taking logarithms):
       0.45 * maglim + 0.95 - 2 * log( ZoomFactor ) - log( Star Density ) - log( Some proportionality constant )
     hence the formula. We've gathered together all the constants and set it to 3.5, so as to set the minimum
     possible value of maglim to 3.5
    */
     
    //    float maglim = 4.444 * ( lgz - lgmin ) + 2.222 * log10( Options::starDensity() ) + 3.5;

    // Reducing the slope w.r.t zoom factor to avoid the extremely fast increase in star density with zoom
    // that 4.444 gives us (although that is what the derivation gives us)

    float maglim = 3.7 * ( lgz - lgmin ) + 2.222 * log10( Options::starDensity() ) + 3.5;

    m_zoomMagLimit = maglim;

    double maxSize = 10.0;

    double labelMagLim = Options::starLabelDensity() / 5.0;
    labelMagLim += ( 12.0 - labelMagLim ) * ( lgz - lgmin) / (lgmax - lgmin );
    if ( labelMagLim > 8.0 ) labelMagLim = 8.0;

//REMOVE
//     //Set the brush
//     QColor fillColor( Qt::white );
//     if ( starColorMode() == 1 ) fillColor = Qt::red;
//     if ( starColorMode() == 2 ) fillColor = Qt::black;
//     psky.setBrush( QBrush( fillColor ) );
//     if ( starColorMode() > 0 )
//         psky.setPen( QPen( fillColor ) );
//     else
//        //Reset the colors before drawing the stars.
//        //Strictly speaking, we don't need to do this every time, but once per
//        //draw loop isn't too expensive.
//        StarObject::updateColors( (! Options::useAntialias() ||
//                                   map->isSlewing()), starColorIntensity() );
//END_REMOVE

    //Loop for drawing star images

    MeshIterator region(m_skyMesh, DRAW_BUF);
    // TODO: Debug code. May not be useful in production. Remove if required.
    magLim = maglim;

    m_StarBlockFactory->drawID = m_skyMesh->drawID();

    QTime t;
    int nTrixels = 0;

    /*
    t_drawNamed = 0;
    t_dynamicLoad = 0;
    t_updateCache = 0;
    t_drawUnnamed = 0;
    */

    visibleStarCount = 0;

    t.start();
    // Old formula:
    //    float sizeMagLim = ( 2.000 + 2.444 * Options::memUsage() / 10.0 ) * ( lgz - lgmin ) + 5.8;

    // Using the maglim to compute the sizes of stars reduces
    // discernability between brighter and fainter stars at high zoom
    // levels. To fix that, we use an "arbitrary" constant in place of
    // the variable star density.
    // Not using this formula now.
    //    float sizeMagLim = 4.444 * ( lgz - lgmin ) + 5.0;

    float sizeMagLim = maglim;

    if( sizeMagLim > m_FaintMagnitude * ( 1 - 1.5/16 ) )
        sizeMagLim = m_FaintMagnitude * ( 1 - 1.5/16 );
    float sizeFactor = 10.0 + (lgz - lgmin);

    while ( region.hasNext() ) {
        ++nTrixels;
        Trixel currentRegion = region.next();
        StarList* starList = m_starIndex->at( currentRegion );
        for (int i=0; i < starList->size(); ++i) {
        
            StarObject *curStar = starList->at( i );
            if( !curStar ) continue;
            
            if ( curStar->updateID != updateID )
                curStar->JITupdate( data );
            
            float mag = curStar->mag();
            
            // break loop if maglim is reached
            if ( mag > maglim || ( hideFaintStars && curStar->mag() > hideStarsMag ) ) {
                //                kDebug() << "Breaking off @ mag = " << mag;
                break;
            }
                 
            //            kDebug() << "Passed mag limit. mag = " << mag;

            if ( ! map->checkVisibility( curStar ) ) continue;
            //            kDebug() << "Passed visibility";
            QPointF o = map->toScreen( curStar );
            
            if ( ! map->onScreen( o ) ) continue;

            float size = ( sizeFactor*( sizeMagLim - mag ) / sizeMagLim ) + 1.;
            if ( size <= 1.0 ) size = 1.0;
            if( size > maxSize ) size = maxSize;

            curStar->draw( psky, o.x(), o.y(), size, (starColorMode()==0),
                           starColorIntensity(), true );
            visibleStarCount++;
            
            if ( m_hideLabels || mag > labelMagLim ) continue;
            
            addLabel( o, curStar );
        }
        //        t_drawNamed += t.restart();
    }

    // Now draw each of our DeepStarComponents
    for( int i =0; i < m_DeepStarComponents.size(); ++i ) {
        m_DeepStarComponents.at( i )->draw( psky );
    }
}

void StarComponent::addLabel( const QPointF& p, StarObject *star )
{
    int idx = int( star->mag() * 10.0 );
    if ( idx < 0 ) idx = 0;
    if ( idx > MAX_LINENUMBER_MAG ) idx = MAX_LINENUMBER_MAG;
    m_labelList[ idx ]->append( SkyLabel( p, star ) );
}

void StarComponent::drawLabels( QPainter& psky )
{
    if ( m_hideLabels ) return;

    psky.setPen( QColor( KStarsData::Instance()->colorScheme()->colorNamed( "SNameColor" ) ) );

    int max = int( m_zoomMagLimit * 10.0 );
    if ( max < 0 ) max = 0;
    if ( max > MAX_LINENUMBER_MAG ) max = MAX_LINENUMBER_MAG;

    for ( int i = 0; i <= max; i++ ) {
        LabelList* list = m_labelList[ i ];
        for ( int j = 0; j < list->size(); j++ ) {
            list->at(j).obj->drawNameLabel( psky, list->at(j).o );
        }
        list->clear();
    }

}

bool StarComponent::loadStaticData()
{
    // We break from Qt / KDE API and use traditional file handling here, to obtain speed.
    // We also avoid C++ constructors for the same reason.
    FILE *dataFile, *nameFile;
    bool swapBytes = false;
    BinFileHelper dataReader, nameReader;
    QString name, gname, visibleName;
    StarObject plainStarTemplate;
    StarObject *star;

    if(starsLoaded)
        return true;

    // prepare to index stars to this date
    m_skyMesh->setKSNumbers( &m_reindexNum );
        
    /* Open the data files */
    // TODO: Maybe we don't want to hardcode the filename?
    if((dataFile = dataReader.openFile("namedstars.dat")) == NULL) {
        kDebug() << "Could not open data file namedstars.dat" << endl;
        return false;
    }

    if(!(nameFile = nameReader.openFile("starnames.dat"))) {
        kDebug() << "Could not open data file starnames.dat" << endl;
        return false;
    }

    if(!dataReader.readHeader()) {
        kDebug() << "Error reading namedstars.dat header : " << dataReader.getErrorNumber() << " : " << dataReader.getError() << endl;
        return false;
    }

    if(!nameReader.readHeader()) {
        kDebug() << "Error reading starnames.dat header : " << nameReader.getErrorNumber() << " : " << nameReader.getError() << endl;
        return false;
    }
    KDE_fseek(nameFile, nameReader.getDataOffset(), SEEK_SET);
    swapBytes = dataReader.getByteSwap();

    long int nstars = 0;
    QTime t;

    // TODO : Remove timing code when we are done with all possible optimizations
    t.start();

    KDE_fseek(dataFile, dataReader.getDataOffset(), SEEK_SET);

    qint16 faintmag;
    quint8 htm_level;
    quint16 t_MSpT;

    fread( &faintmag, 2, 1, dataFile );
    fread( &htm_level, 1, 1, dataFile );
    fread( &t_MSpT, 2, 1, dataFile ); // Unused

    if( faintmag / 100.0 > m_FaintMagnitude )
        m_FaintMagnitude = faintmag / 100.0;

    if( htm_level != m_skyMesh->level() )
        kDebug() << "WARNING: HTM Level in shallow star data file and HTM Level in m_skyMesh do not match. EXPECT TROUBLE" << endl;

    for(int i = 0; i < m_skyMesh -> size(); ++i) {

        Trixel trixel = i;// = ( ( i >= 256 ) ? ( i - 256 ) : ( i + 256 ) );
        for(unsigned long j = 0; j < (unsigned long)dataReader.getRecordCount(i); ++j) {
            bool named = false;
            if(!fread(&stardata, sizeof(starData), 1, dataFile)){
                kDebug() << "FILE FORMAT ERROR: Could not read starData structure for star #" << j << " under trixel #" << trixel << endl;
            }

            /* Swap Bytes when required */            
            if(swapBytes)
                byteSwap( &stardata );

            if(stardata.flags & 0x01) {
                /* Named Star - Read the nameFile */
                visibleName = "";
                if(!fread(&starname, sizeof( starName ), 1, nameFile))
                    kDebug() << "ERROR: fread() call on nameFile failed in trixel " << trixel << " star " << j << endl;
                name = QByteArray(starname.longName, 32);
                gname = QByteArray(starname.bayerName, 8);
                if ( ! gname.isEmpty() && gname.at(0) != '.')
                    visibleName = gname;
                if(! name.isEmpty() ) {
                    // HEV: look up star name in internationalization filesource
                    name = i18nc("star name", name.toLocal8Bit().data());
                }
                else
                    name = i18n("star");
                named = true;
            }
            else
                kDebug() << "ERROR: Named star file contains unnamed stars! Expect trouble." << endl;

            /* Create the new StarObject */
            star = new StarObject;
            star->init( &stardata );
            star->setNames( name, visibleName );
            star->EquatorialToHorizontal( data()->lst(), data()->geo()->lat() );
            ++nstars;
            
            if ( ! gname.isEmpty() ) m_genName.insert( gname, star );
                
            if ( ! name.isEmpty() ) {
                objectNames(SkyObject::STAR).append( name );
            }
            if ( ! gname.isEmpty() && gname != name ) {
                objectNames(SkyObject::STAR).append( star -> gname(false) );
            }
                
            objectList().append( star );
                
            m_starIndex->at( trixel )->append( star );
            double pm = star->pmMagnitude();
            for (int j = 0; j < m_highPMStars.size(); j++ ) {
                HighPMStarList* list = m_highPMStars.at( j );
                if ( list->append( trixel, star, pm ) ) break;
            }

            if( star->getHDIndex() != 0 )
                m_HDHash.insert( star->getHDIndex(), star );
        }

    }

    dataReader.closeFile();
    nameReader.closeFile();
    kDebug() << "Loaded " << nstars << " stars in " << t.elapsed() << " ms" << endl;

    starsLoaded = true;
    return true;

}

SkyObject* StarComponent::findStarByGenetiveName( const QString name ) {
    return m_genName.value( name );
}

// Overrides ListComponent::findByName() to include genetive name and HD index also in the search
SkyObject* StarComponent::findByName( const QString &name ) {
    SkyObject *o;
    foreach ( o, objectList() )
        if ( QString::compare( o->name(), name, Qt::CaseInsensitive ) == 0 || 
             QString::compare( o->longname(), name, Qt::CaseInsensitive ) == 0 || 
             QString::compare( o->name2(), name, Qt::CaseInsensitive ) == 0 || 
             QString::compare( ((StarObject *)o)->gname(false), name, Qt::CaseInsensitive ) == 0)
            return o;

    return 0;
}

// TODO: Strongly consider including Deep Star Components inside StarComponent
SkyObject *StarComponent::findByHDIndex( int HDnum ) {
    SkyObject *o;
    BinFileHelper hdidxReader;
    // First check the hash to see if we have a corresponding StarObject already
    if( ( o = m_HDHash.value( HDnum, NULL ) ) )
        return o;
    // If we don't have the StarObject here, try it in the DeepStarComponents' hashes
    if( m_DeepStarComponents.size() >= 1 )
        if( ( o = m_DeepStarComponents.at( 0 )->findByHDIndex( HDnum ) ) )
            return o;
    if( m_DeepStarComponents.size() >= 2 ) {
        int32_t offset;
        FILE *hdidxFile = hdidxReader.openFile( "Henry-Draper.idx" );
        if( !hdidxFile )
            return 0;
        FILE *dataFile;
        KDE_fseek( hdidxFile, (HDnum - 1) * 4, SEEK_SET );
        fread( &offset, 4, 1, hdidxFile );
        if( offset <= 0 )
            return 0;
        // TODO: Implement byteswapping
        dataFile = m_DeepStarComponents.at( 1 )->getStarReader()->getFileHandle();
        KDE_fseek( dataFile, offset, SEEK_SET );
        fread( &stardata, sizeof( starData ), 1, dataFile );
        // TODO: Implement byteswapping
        // byteSwap( &stardata );
        m_starObject.init( &stardata );
        m_starObject.EquatorialToHorizontal( data()->lst(), data()->geo()->lat() );
        m_starObject.JITupdate( data() );
        hdidxReader.closeFile();
        // TODO: Lots of trouble since we are returning a copy. Can we fix that?
        return &m_starObject;
    }
        
    return 0;
}

// This uses the main star index for looking up nearby stars but then
// filters out objects with the generic name "star".  We could easily
// build an index for just the named stars which would make this go
// much faster still.  -jbb
//
SkyObject* StarComponent::objectNearest( SkyPoint *p, double &maxrad )
{
    SkyObject *oBest = 0;

    MeshIterator region( m_skyMesh, OBJ_NEAREST_BUF );

    while ( region.hasNext() ) {
        Trixel currentRegion = region.next();
        StarList* starList = m_starIndex->at( currentRegion );
        for (int i=0; i < starList->size(); ++i) {
            StarObject* star =  starList->at( i );
            if( !star ) continue;
            if ( star->mag() > m_zoomMagLimit ) continue;
            
            double r = star->angularDistanceTo( p ).Degrees();
            if ( r < maxrad ) {
                oBest = star;
                maxrad = r;
            }
        }
    }

    // Check up with our Deep Star Components too!
    double rTry, rBest;
    SkyObject *oTry;
    rBest = maxrad;
    rTry = maxrad;
    for( int i = 0; i < m_DeepStarComponents.size(); ++i ) {
      oTry = m_DeepStarComponents.at( i )->objectNearest( p, rTry );
      // TODO: Should we multiply rBest by a factor < 1, so that we give higher priority to named stars?
      if( rTry < rBest ) {
	rBest = rTry;
	oBest = oTry;
      }
    }
    maxrad = rBest;

    return (SkyObject*) oBest;
}

int StarComponent::starColorMode( void ) const {
    return m_Data->colorScheme()->starColorMode();
}

int StarComponent::starColorIntensity( void ) const {
    return m_Data->colorScheme()->starColorIntensity();
}

void StarComponent::byteSwap( starData *stardata ) {
    bswap_32( stardata->RA );
    bswap_32( stardata->Dec );
    bswap_32( stardata->dRA );
    bswap_32( stardata->dDec );
    bswap_32( stardata->parallax );
    bswap_32( stardata->HD );
    bswap_16( stardata->mag );
    bswap_16( stardata->bv_index );
}
/*
void StarComponent::printDebugInfo() {

    int nTrixels = 0;
    int nBlocks = 0;
    long int nStars = 0;
    float faintMag = -5.0;

    MeshIterator trixels( m_skyMesh, DRAW_BUF );
    Trixel trixel;

    while( trixels.hasNext() ) {
        trixel = trixels.next();
        nTrixels++;
        for(int i = 0; i < m_starBlockList[ trixel ]->getBlockCount(); ++i) {
            nBlocks++;
            StarBlock *block = m_starBlockList[ trixel ]->block( i );
            for(int j = 0; j < block->getStarCount(); ++j) {
                nStars++;
            }
            if( block->getFaintMag() > faintMag ) {
                faintMag = block->getFaintMag();
            }
        }
    }

    printf( "========== UNNAMED STAR MEMORY ALLOCATION INFORMATION ==========\n" );
    printf( "Number of visible trixels                    = %8d\n", nTrixels );
    printf( "Number of visible StarBlocks                 = %8d\n", nBlocks );
    printf( "Number of StarBlocks allocated via SBF       = %8d\n", m_StarBlockFactory.getBlockCount() );
    printf( "Number of unnamed stars in memory            = %8ld\n", nStars );
    printf( "Number of visible stars (named + unnamed)    = %8ld\n", visibleStarCount );
    printf( "Magnitude of the faintest star in memory     = %8.2f\n", faintMag );
    printf( "Target magnitude limit                       = %8.2f\n", magLim );
    printf( "Size of each StarBlock                       = %8d bytes\n", sizeof( StarBlock ) );
    printf( "Size of each StarObject                      = %8d bytes\n", sizeof( StarObject ) );
    printf( "Memory use due to visible unnamed stars      = %8.2f MB\n", ( sizeof( StarObject ) * nStars / 1048576.0 ) );
    printf( "Memory use due to visible StarBlocks         = %8d bytes\n", sizeof( StarBlock ) * nBlocks );
    printf( "Memory use due to StarBlocks in SBF          = %8d bytes\n", sizeof( StarBlock ) * m_StarBlockFactory.getBlockCount() );
    printf( "=============== STAR DRAW LOOP TIMING INFORMATION ==============\n" );
    printf( "Time taken for drawing named stars           = %8ld ms\n", t_drawNamed );
    printf( "Time taken for dynamic load of data          = %8ld ms\n", t_dynamicLoad );
    printf( "Time taken for updating LRU cache            = %8ld ms\n", t_updateCache );
    printf( "Time taken for drawing unnamed stars         = %8ld ms\n", t_drawUnnamed );
    printf( "Average Number of stars draw per millisecond = %8.2f stars/ms\n", ( (double)visibleStarCount ) / ( t_drawUnnamed + t_drawNamed ) );
    printf( "================================================================\n" );
}

bool StarComponent::verifySBLIntegrity() {

    float faintMag = -5.0;
    bool integrity = true;
    for(Trixel trixel = 0; trixel < m_skyMesh->size(); ++trixel) {
        for(int i = 0; i < m_starBlockList[ trixel ]->getBlockCount(); ++i) {
            StarBlock *block = m_starBlockList[ trixel ]->block( i );
            if( i == 0 )
                faintMag = block->getBrightMag();
            // NOTE: Assumes 2 decimal places in magnitude field. TODO: Change if it ever does change
            if( block->getBrightMag() != faintMag && ( block->getBrightMag() - faintMag ) > 0.016) {
                kDebug() << "Trixel " << trixel << ": ERROR: faintMag of prev block = " << faintMag 
                         << ", brightMag of block #" << i << " = " << block->getBrightMag();
                integrity = false;
            }
            if( i > 1 && ( !block->prev ) )
                kDebug() << "Trixel " << trixel << ": ERROR: Block" << i << "is unlinked in LRU Cache";
            if( block->prev && block->prev->parent == m_starBlockList[ trixel ] 
                && block->prev != m_starBlockList[ trixel ]->block( i - 1 ) ) {
                kDebug() << "Trixel " << trixel << ": ERROR: SBF LRU Cache linked list seems to be broken at before block " << i << endl;
                integrity = false;
            }
            faintMag = block->getFaintMag();
        }
    }
    return integrity;
}
*/
