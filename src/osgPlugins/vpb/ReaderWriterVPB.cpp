/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/MapConfig>
#include <osgEarth/Mercator>

#include <osg/Notify>
#include <osg/io_utils>
#include <osg/Version>

#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

#include <sstream>

using namespace osgEarth;

#define PROPERTY_URL                    "url"
#define PROPERTY_PRIMARY_SPLIT_LEVEL    "primary_split_level"
#define PROPERTY_SECONDARY_SPLIT_LEVEL  "secondary_split_level"
#define PROPERTY_DIRECTORY_STRUCTURE    "directory_structure"
#define PROPERTY_LAYER_NUM              "layer"
#define PROPERTY_NUM_TILES_WIDE_AT_LOD0 "num_tiles_wide_at_lod0"
#define PROPERTY_NUM_TILES_HIGH_AT_LOD0 "num_tiles_high_at_lod0"

int getLOD(const osgTerrain::TileID& id)
{
    //The name of the lod changed after OSG 2.6 from layer to level
#if (OPENSCENEGRAPH_MAJOR_VERSION == 2 && OPENSCENEGRAPH_MINOR_VERSION < 7)
    return id.layer;
#else
    return id.level;
#endif
}


class CollectTiles : public osg::NodeVisitor
{
public:

    CollectTiles(): 
        osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
    {
    }
    
    void reset()
    {
        _terrainTiles.clear();
    }
    
    void apply(osg::Group& group)
    {
        osgTerrain::TerrainTile* terrainTile = dynamic_cast<osgTerrain::TerrainTile*>(&group);
        if (terrainTile)
        {
            osg::notify(osg::INFO)<<"Found terrain tile TileID("<<
                getLOD(terrainTile->getTileID())<<", "<<
                terrainTile->getTileID().x<<", "<<
                terrainTile->getTileID().y<<")"<<std::endl;
            
            _terrainTiles.push_back(terrainTile);
        }
        else
        {
            traverse(group);
        }
    }
    
    osgTerrain::Locator* getLocator()
    {
        for(unsigned int i=0; i<_terrainTiles.size(); ++i)
        {
            osgTerrain::TerrainTile* tile = _terrainTiles[i].get();
            osgTerrain::Locator* locator = tile->getLocator();
            if (locator) return locator;
        }
        return 0;
    }

    bool getRange(double& min_x, double& min_y, double& max_x, double& max_y) const
    {
        min_x = DBL_MAX;
        max_x = -DBL_MAX;
        min_y = DBL_MAX;
        max_y = -DBL_MAX;
        
        typedef std::vector<osg::Vec3d> Corners;
        Corners corners;
        corners.push_back(osg::Vec3d(0.0f,0.0f,0.0f));
        corners.push_back(osg::Vec3d(1.0f,0.0f,0.0f));
        corners.push_back(osg::Vec3d(1.0f,1.0f,0.0f));
        corners.push_back(osg::Vec3d(1.0f,1.0f,0.0f));
        
        for(unsigned int i=0; i<_terrainTiles.size(); ++i)
        {
            osgTerrain::TerrainTile* tile = _terrainTiles[i].get();
            osgTerrain::Locator* locator = tile->getLocator();
            if (locator)
            {
                for(Corners::iterator itr = corners.begin();
                    itr != corners.end();
                    ++itr)
                {
                    osg::Vec3d& local = *itr;
                    osg::Vec3d projected = local * locator->getTransform();

                    if (projected.x()<min_x) min_x = projected.x();
                    if (projected.x()>max_x) max_x = projected.x();

                    if (projected.y()<min_y) min_y = projected.y();
                    if (projected.y()>max_y) max_y = projected.y();
                }
            }
        }
        
        return min_x <= max_x;
    }
    
    typedef std::vector< osg::ref_ptr<osgTerrain::TerrainTile> > TerrainTiles;
    TerrainTiles _terrainTiles;
};


class VPBDatabase : public osg::Referenced
{
public:

    enum DirectoryStructure
    {
        FLAT,
        FLAT_TASK_DIRECTORIES,
        NESTED_TASK_DIRECTORIES
    };

    VPBDatabase(const osgDB::ReaderWriter::Options* in_options ) :
      options( in_options),
      primary_split_level(-1),
      secondary_split_level(-1),
      directory_structure(FLAT_TASK_DIRECTORIES),
      maxNumTilesInCache(128),
      profile( TileGridProfile::GLOBAL_GEODETIC )
    {
    
        unsigned int numTilesWideAtLod0 = profile.getNumTilesWideAtLod0();
        unsigned int numTilesHighAtLod0 = profile.getNumTilesHighAtLod0();

        if ( options.valid() )
        {
            if ( options->getPluginData( PROPERTY_URL ) )
                url = std::string( (const char*)options->getPluginData( PROPERTY_URL ) );

            if ( options->getPluginData( PROPERTY_PRIMARY_SPLIT_LEVEL ) )
                primary_split_level = atoi( (const char*)options->getPluginData( PROPERTY_PRIMARY_SPLIT_LEVEL ) );

            if ( options->getPluginData( PROPERTY_SECONDARY_SPLIT_LEVEL ) )
                secondary_split_level = atoi( (const char*)options->getPluginData( PROPERTY_SECONDARY_SPLIT_LEVEL ) );

            if ( options->getPluginData( PROPERTY_DIRECTORY_STRUCTURE ) )
            {
                std::string dir_string((const char*)options->getPluginData( PROPERTY_DIRECTORY_STRUCTURE ));
                if (dir_string=="nested" || dir_string=="nested_task_directories" ) directory_structure = NESTED_TASK_DIRECTORIES;
                if (dir_string=="task" || dir_string=="flat_task_directories") directory_structure = FLAT_TASK_DIRECTORIES;
                if (dir_string=="flat") directory_structure = FLAT;
                
                osg::notify(osg::NOTICE)<<"dir_string="<<dir_string<<" result "<<directory_structure<<std::endl;
            }
        }

        // validate dataset
        if (!url.empty())
        {
            root_node = osgDB::readNodeFile(url);
            
            if (root_node.valid())
            {
                path = osgDB::getFilePath(url);
                base_name = osgDB::getStrippedName(url);
                extension = osgDB::getFileExtension(url);
                
                osg::notify(osg::INFO)<<"VPBasebase constructor: Loaded root "<<url<<", path="<<path<<" base_name="<<base_name<<" extension="<<extension<<std::endl;
                
                std::string srs = profile.srs();
                
                osg::CoordinateSystemNode* csn = dynamic_cast<osg::CoordinateSystemNode*>(root_node.get());
                if (csn)
                {
                    osg::notify(osg::INFO)<<"CordinateSystemNode found, coordinate system is : "<<csn->getCoordinateSystem()<<std::endl;
                    
                    srs = csn->getCoordinateSystem();
                }

                CollectTiles ct;
                root_node->accept(ct);

                    
                osgTerrain::Locator* locator = ct.getLocator();
                if (locator)
                {
                    double min_x, max_x, min_y, max_y;
                    ct.getRange(min_x, min_y, max_x, max_y);

                    osg::notify(osg::INFO)<<"range("<<min_x<<", "<<min_y<<", "<<max_x<<", "<<max_y<<std::endl;

                    srs = locator->getCoordinateSystem();

                    profile = osgEarth::TileGridProfile( 
                        osg::RadiansToDegrees(min_x), 
                        osg::RadiansToDegrees(min_y), 
                        osg::RadiansToDegrees(max_x), 
                        osg::RadiansToDegrees(max_y),
                        srs);
                
                    switch(locator->getCoordinateSystemType())
                    {
                        case(osgTerrain::Locator::GEOCENTRIC):
                            profile.setProfileType(osgEarth::TileGridProfile::GLOBAL_GEODETIC);
                            break;
                        case(osgTerrain::Locator::GEOGRAPHIC):
                            profile.setProfileType(osgEarth::TileGridProfile::PROJECTED);
                            break;
                        case(osgTerrain::Locator::PROJECTED):
                            profile.setProfileType(osgEarth::TileGridProfile::PROJECTED);
                            break;
                    }
                    double aspectRatio = (max_x-min_x)/(max_y-min_y);
                    
                    osg::notify(osg::INFO)<<"aspectRatio = "<<aspectRatio<<std::endl;

                    if (aspectRatio>1.0)
                    {
                        numTilesWideAtLod0 = static_cast<unsigned int>(floor(aspectRatio+0.499999));
                        numTilesHighAtLod0 = 1;
                    }
                    else
                    {
                        numTilesWideAtLod0 = 1;
                        numTilesHighAtLod0 = static_cast<unsigned int>(floor(1.0/aspectRatio+0.499999));
                    }
                    
                    osg::notify(osg::INFO)<<"computed numTilesWideAtLod0 = "<<numTilesWideAtLod0<<std::endl;
                    osg::notify(osg::INFO)<<"computed numTilesHightAtLod0 = "<<numTilesHighAtLod0<<std::endl;
                    
                    if ( options.valid() )
                    {
                        if ( options->getPluginData( PROPERTY_NUM_TILES_WIDE_AT_LOD0 ) )
                            numTilesWideAtLod0 = atoi( (const char*)options->getPluginData( PROPERTY_NUM_TILES_WIDE_AT_LOD0 ) );

                        if ( options->getPluginData( PROPERTY_NUM_TILES_HIGH_AT_LOD0 ) )
                            numTilesHighAtLod0 = atoi( (const char*)options->getPluginData( PROPERTY_NUM_TILES_HIGH_AT_LOD0 ) );
                    }

                    osg::notify(osg::INFO)<<"final numTilesWideAtLod0 = "<<numTilesWideAtLod0<<std::endl;
                    osg::notify(osg::INFO)<<"final numTilesHightAtLod0 = "<<numTilesHighAtLod0<<std::endl;

                    profile.setNumTilesWideAtLod0(numTilesWideAtLod0);
                    profile.setNumTilesHighAtLod0(numTilesHighAtLod0);

                }
                
            }
            else
            {
                osg::notify(osg::NOTICE)<<"Unable to read file "<<url<<std::endl;
                url = "";
            }
        }
        else 
        {
            osg::notify(osg::NOTICE)<<"No data referenced "<<std::endl;
        }

    }
    
    std::string createTileName( int level, int tile_x, int tile_y )
    {
        std::stringstream buf;
        if (directory_structure==FLAT)
        {
             buf<<path<<"/"<<base_name<<"_L"<<level<<"_X"<<tile_x/2<<"_Y"<<tile_y/2<<"_subtile."<<extension;
        }
        else
        {
            if (level<primary_split_level)
            {
                buf<<path<<"/"<<base_name<<"_root_L0_X0_Y0/"<<
                     base_name<<"_L"<<level<<"_X"<<tile_x/2<<"_Y"<<tile_y/2<<"_subtile."<<extension;

            }
            else if (level<secondary_split_level)
            {
                tile_x /= 2;
                tile_y /= 2;

                int split_x = tile_x >> (level - primary_split_level);
                int split_y = tile_y >> (level - primary_split_level);

                buf<<path<<"/"<<base_name<<"_subtile_L"<<primary_split_level<<"_X"<<split_x<<"_Y"<<split_y<<"/"<<
                     base_name<<"_L"<<level<<"_X"<<tile_x<<"_Y"<<tile_y<<"_subtile."<<extension;
            }
            else if (directory_structure==NESTED_TASK_DIRECTORIES)
            {
                tile_x /= 2;
                tile_y /= 2;

                int split_x = tile_x >> (level - primary_split_level);
                int split_y = tile_y >> (level - primary_split_level);

                int secondary_split_x = tile_x >> (level - secondary_split_level);
                int secondary_split_y = tile_y >> (level - secondary_split_level);

                buf<<path<<"/"<<base_name<<"_subtile_L"<<primary_split_level<<"_X"<<split_x<<"_Y"<<split_y<<"/"<<
                     base_name<<"_subtile_L"<<secondary_split_level<<"_X"<<secondary_split_x<<"_Y"<<secondary_split_y<<"/"<< 
                     base_name<<"_L"<<level<<"_X"<<tile_x<<"_Y"<<tile_y<<"_subtile."<<extension;
            }
            else
            {
                tile_x /= 2;
                tile_y /= 2;

                int split_x = tile_x >> (level - secondary_split_level);
                int split_y = tile_y >> (level - secondary_split_level);

                buf<<path<<"/"<<base_name<<"_subtile_L"<<secondary_split_level<<"_X"<<split_x<<"_Y"<<split_y<<"/"<<
                     base_name<<"_L"<<level<<"_X"<<tile_x<<"_Y"<<tile_y<<"_subtile."<<extension;
            }
        }
        
        osg::notify(osg::INFO)<<"VPBDatabase::createTileName(), buf.str()=="<<buf.str()<<std::endl;
        
        return buf.str();
    }
    
    osgTerrain::TerrainTile* getTerrainTile(const TileKey* key )
    {
        int level = key->getLevelOfDetail();
        unsigned int tile_x, tile_y;
        key->getTileXY( tile_x, tile_y );
        
        int max_x = (2 << level) - 1;
        int max_y = (1 << level) - 1;
        
        tile_y = max_y - tile_y;

        osgTerrain::TileID tileID(level, tile_x, tile_y);

        osgTerrain::TerrainTile* tile = findTile(tileID, false);
        if (tile) return tile;
        
        osg::notify(osg::INFO)<<"Max_x = "<<max_x<<std::endl;
        osg::notify(osg::INFO)<<"Max_y = "<<max_y<<std::endl;

        osg::notify(osg::INFO)<<"base_name = "<<base_name<<" psl="<<primary_split_level<<" ssl="<<secondary_split_level<<std::endl;
        osg::notify(osg::INFO)<<"level = "<<level<<", x = "<<tile_x<<", tile_y = "<<tile_y<<std::endl;
        osg::notify(osg::INFO)<<"tile_name "<<createTileName(level, tile_x, tile_y)<<std::endl;
        osg::notify(osg::INFO)<<"thread "<<OpenThreads::Thread::CurrentThread()<<std::endl;

        std::string filename = createTileName(level, tile_x, tile_y);
        
        
        if (blacklistedFilenames.count(filename)==1)
        {
            osg::notify(osg::INFO)<<"file has been found in black list : "<<filename<<std::endl;

            insertTile(tileID, 0);
            return 0;
        }
        
        osg::ref_ptr<osg::Node> node = osgDB::readNodeFile(filename);

        if (node.valid())
        {
            osg::notify(osg::INFO)<<"Loaded model "<<filename<<std::endl;
            CollectTiles ct;
            node->accept(ct);

            int base_x = (tile_x / 2) * 2;
            int base_y = (tile_y / 2) * 2;
            
            double min_x, max_x, min_y, max_y;
            ct.getRange(min_x, min_y, max_x, max_y);

            double center_x = (min_x + max_x)*0.5;
            double center_y = (min_y + max_y)*0.5;

            osg::Vec3d local(0.5,0.5,0.0);            
            for(unsigned int i=0; i<ct._terrainTiles.size(); ++i)
            {
                osgTerrain::TerrainTile* tile = ct._terrainTiles[i].get();
                osgTerrain::Locator* locator = tile->getLocator();
                if (locator)
                {
                    osg::Vec3d projected = local * locator->getTransform();
                    
                    int local_x = base_x + ((projected.x() > center_x) ? 1 : 0);
                    int local_y = base_y + ((projected.y() > center_y) ? 1 : 0);
                    osgTerrain::TileID local_tileID(level, local_x, local_y);
                    
                    tile->setTileID(local_tileID);
                    insertTile(local_tileID, tile);                    
                }

            }

        }
        else
        {
            osg::notify(osg::INFO)<<"Black listing : "<<filename<<std::endl;
            blacklistedFilenames.insert(filename);
        }
        
        return findTile(tileID, true);
    }
    
    void insertTile(const osgTerrain::TileID& tileID, osgTerrain::TerrainTile* tile)
    {
        tileMap[tileID] = tile;
        
        tileFIFO.push_back(tileID);
        
        if (tileFIFO.size()>maxNumTilesInCache)
        {
            osgTerrain::TileID tileToRemove = tileFIFO.front();
            tileFIFO.pop_front();            
            tileMap.erase(tileToRemove);
            
            osg::notify(osg::INFO)<<"Pruned tileID ("<<getLOD(tileID)<<", "<<tileID.x<<", "<<tileID.y<<")"<<std::endl;
        }
        
        osg::notify(osg::INFO)<<"insertedTile tileFIFO.size()=="<<tileFIFO.size()<<std::endl;
    }

    osgTerrain::TerrainTile* findTile(const osgTerrain::TileID& tileID, bool insertBlankTileIfNotFound = false)
    {
        TileMap::iterator itr = tileMap.find(tileID);
        if (itr!=tileMap.end()) return itr->second.get();

        if (insertBlankTileIfNotFound) insertTile(tileID, 0);
        
        return 0;
    }

    osg::ref_ptr<const osgDB::ReaderWriter::Options> options;
    std::string url;
    std::string path;
    std::string base_name;
    std::string extension;
    int primary_split_level;
    int secondary_split_level;
    DirectoryStructure directory_structure;

    TileGridProfile profile;
    osg::ref_ptr<osg::Node> root_node;
    
    unsigned int maxNumTilesInCache;
    
    typedef std::map<osgTerrain::TileID, osg::ref_ptr<osgTerrain::TerrainTile> > TileMap;
    TileMap tileMap;
    
    typedef std::list<osgTerrain::TileID> TileIDList;
    TileIDList tileFIFO;
    
    typedef std::set<std::string> StringSet;
    StringSet blacklistedFilenames;
    
};

class VPBSource : public TileSource
{
public:
    VPBSource( VPBDatabase* vpbDatabase, const osgDB::ReaderWriter::Options* in_options) :
        vpbDatabase(vpbDatabase),
        options(in_options),
        layerNum(0)
    {
        if ( options.valid() )
        {
            if ( options->getPluginData( PROPERTY_LAYER_NUM ) )
                layerNum = atoi( (const char*)options->getPluginData( PROPERTY_LAYER_NUM ) );
        }
    }

    const TileGridProfile& getProfile() const
    {
        return vpbDatabase->profile;
    }
    
    osg::Image* createImage( const TileKey* key )
    {
        osgTerrain::TerrainTile* tile = vpbDatabase->getTerrainTile(key);                
        if (tile)
        {        
            if (layerNum < tile->getNumColorLayers())
            {
                osgTerrain::Layer* layer = tile->getColorLayer(layerNum);
                osgTerrain::ImageLayer* imageLayer = dynamic_cast<osgTerrain::ImageLayer*>(layer);
                if (imageLayer)
                {
                    return imageLayer->getImage();
                }
            }
        }
        
        return 0;
    }

    osg::HeightField* createHeightField( const TileKey* key )
    {
        osgTerrain::TerrainTile* tile = vpbDatabase->getTerrainTile(key);                
        if (tile)
        {        
            osgTerrain::Layer* elevationLayer = tile->getElevationLayer();
            osgTerrain::HeightFieldLayer* hfLayer = dynamic_cast<osgTerrain::HeightFieldLayer*>(elevationLayer);
            if (hfLayer) 
            {
                return hfLayer->getHeightField();
            }
        }

        return 0;
    }

    virtual std::string getExtension()  const 
    {
        //All VPB tiles are in IVE format
        return vpbDatabase->extension;
    }

private:
    osg::ref_ptr<VPBDatabase>                           vpbDatabase;
    osg::ref_ptr<const osgDB::ReaderWriter::Options>    options;
    unsigned int                                        layerNum;
};


class ReaderWriterVPB : public osgDB::ReaderWriter
{
    public:
        ReaderWriterVPB()
        {
            supportsExtension( "osgearth_vpb", "VirtualPlanetBuilder data" );
        }

        virtual const char* className()
        {
            return "VirtualPlanetBuilder ReaderWriter";
        }

        virtual ReadResult readObject(const std::string& file_name, const Options* options) const
        {
            if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
                return ReadResult::FILE_NOT_HANDLED;

            if ( options->getPluginData( PROPERTY_URL ) )
            {
                std::string url = std::string( (const char*)options->getPluginData( PROPERTY_URL ) );
                
                osg::observer_ptr<VPBDatabase>& db_ptr = vpbDatabaseMap[url];
                
                if (!db_ptr) db_ptr = new VPBDatabase(options);
                
                if (db_ptr.valid()) return new VPBSource(db_ptr.get(), options);
                else return ReadResult::FILE_NOT_FOUND;
               
            }
            else
            {
                return ReadResult::FILE_NOT_HANDLED;
            }
        }
        
        typedef std::map<std::string, osg::observer_ptr<VPBDatabase> > VPBDatabaseMap;
        mutable VPBDatabaseMap vpbDatabaseMap;
};

REGISTER_OSGPLUGIN(osgearth_vpb, ReaderWriterVPB)
