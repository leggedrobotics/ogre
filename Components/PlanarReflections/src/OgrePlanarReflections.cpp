/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2017 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreStableHeaders.h"

#include "OgrePlanarReflections.h"
#include "OgreSceneManager.h"
#include "OgreTextureManager.h"
#include "OgreHardwarePixelBuffer.h"
#include "OgreRenderTexture.h"
#include "Compositor/OgreCompositorManager2.h"
#include "Compositor/OgreCompositorWorkspace.h"
#include "Math/Array/OgreBooleanMask.h"
#include "OgreHlmsDatablock.h"
#include "OgreHlms.h"
#include "OgreLogManager.h"

namespace Ogre
{
    static const Ogre::Matrix4 PROJECTIONCLIPSPACE2DTOIMAGESPACE_PERSPECTIVE(
            0.5,    0,    0,  0.5,
            0,   -0.5,    0,  0.5,
            0,      0,    1,    0,
            0,      0,    0,    1 );

    PlanarReflections::PlanarReflections( SceneManager *sceneManager,
                                          CompositorManager2 *compositorManager,
                                          uint8 maxActiveActors, Real maxDistance,
                                          Camera *lockCamera ) :
        mActorsSoA( 0 ),
        mCapacityActorsSoA( 0 ),
        mLastAspectRatio( 0 ),
        mLastCameraPos( Vector3::ZERO ),
        mLastCameraRot( Quaternion::IDENTITY ),
        mLastCamera( 0 ),
        //mLockCamera( lockCamera ),
        mUpdatingRenderablesHlms( false ),
        mAnyPendingFlushRenderable( false ),
        mMaxActiveActors( maxActiveActors ),
        mInvMaxDistance( Real(1.0) / maxDistance ),
        mMaxSqDistance( maxDistance * maxDistance ),
        mSceneManager( sceneManager ),
        mCompositorManager( compositorManager )
    {
    }
    //-----------------------------------------------------------------------------------
    PlanarReflections::~PlanarReflections()
    {
        destroyAllActors();
        if( mActorsSoA )
        {
            OGRE_FREE_SIMD( mActorsSoA, MEMCATEGORY_SCENE_OBJECTS );
            mActorsSoA = 0;
            mCapacityActorsSoA = 0;
        }
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::setMaxDistance( Real maxDistance )
    {
        mMaxSqDistance = maxDistance * maxDistance;
        mInvMaxDistance = Real(1.0) / maxDistance;
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::setMaxActiveActors( uint8 maxActiveActors )
    {
        mMaxActiveActors = maxActiveActors;
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::pushActor( PlanarReflectionActor *actor )
    {
        mActors.push_back( actor );

        if( mCapacityActorsSoA < mActors.capacity() )
        {
            if( mActorsSoA )
            {
                OGRE_FREE_SIMD( mActorsSoA, MEMCATEGORY_SCENE_OBJECTS );
                mActorsSoA = 0;
                mCapacityActorsSoA = 0;
            }

            mCapacityActorsSoA = mActors.capacity();

            const size_t bytesRequired = sizeof(ArrayActorPlane) *
                                         alignToNextMultiple( mCapacityActorsSoA, ARRAY_PACKED_REALS ) /
                                         ARRAY_PACKED_REALS;
            mActorsSoA = reinterpret_cast<ArrayActorPlane*>(
                             OGRE_MALLOC_SIMD( bytesRequired, MEMCATEGORY_SCENE_OBJECTS ) );

            mActors.pop_back();

            PlanarReflectionActorVec::const_iterator itor = mActors.begin();
            PlanarReflectionActorVec::const_iterator end  = mActors.end();

            while( itor != end )
            {
                const size_t i = itor - mActors.begin();
                (*itor)->mIndex         = i % ARRAY_PACKED_REALS;
                (*itor)->mActorPlane    = mActorsSoA + (i / ARRAY_PACKED_REALS);
                (*itor)->updateArrayActorPlane();
                ++itor;
            }

            mActors.push_back( actor );
        }

        actor->mIndex       = (mActors.size() - 1u) % ARRAY_PACKED_REALS;
        actor->mActorPlane  = mActorsSoA + ( (mActors.size() - 1u) / ARRAY_PACKED_REALS );
    }
    //-----------------------------------------------------------------------------------
    PlanarReflectionActor* PlanarReflections::addActor( const PlanarReflectionActor &actor,
                                                        bool useAccurateLighting,
                                                        uint32 width, uint32 height, bool withMipmaps,
                                                        PixelFormat pixelFormat,
                                                        bool mipmapMethodCompute )
    {
        const size_t uniqueId = Id::generateNewId<PlanarReflections>();

        pushActor( new PlanarReflectionActor( actor ) );
        PlanarReflectionActor *newActor = mActors.back();
        String cameraName = "PlanarReflectionActor #" + StringConverter::toString( uniqueId );
        newActor->mReflectionCamera = mSceneManager->createCamera( cameraName, useAccurateLighting );
        newActor->mReflectionCamera->setAutoAspectRatio( false );

        int usage = TU_RENDERTARGET;
        usage |= (withMipmaps && mipmapMethodCompute) ? TU_UAV : TU_AUTOMIPMAP;
        const uint32 numMips = withMipmaps ? 0 : PixelUtil::getMaxMipmapCount( width, height, 1u );

        newActor->mReflectionTexture =
                TextureManager::getSingleton().createManual(
                    "PlanarReflections #" + StringConverter::toString( uniqueId ),
                    ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
                    TEX_TYPE_2D, width, height, numMips, pixelFormat, usage, 0, true );

        CompositorChannel channel;
        channel.textures.push_back( newActor->mReflectionTexture );
        channel.target = newActor->mReflectionTexture->getBuffer()->getRenderTarget();
        CompositorChannelVec channels;
        channels.push_back( channel );
        newActor->mWorkspace = mCompositorManager->addWorkspace( mSceneManager, channels,
                                                                 newActor->mReflectionCamera,
                                                                 newActor->mWorkspaceName, false, 0 );
        newActor->updateArrayActorPlane();
        return newActor;
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::destroyActor( PlanarReflectionActor *actor )
    {
        PlanarReflectionActorVec::iterator itor = std::find( mActors.begin(), mActors.end(), actor );

        if( itor == mActors.end() )
        {
            OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND,
                         "Actor was not created by this PlanarReflections class "
                         "or was already destroyed!", "PlanarReflections::destroyActor" );
        }

        mCompositorManager->removeWorkspace( actor->mWorkspace );
        mSceneManager->destroyCamera( actor->mReflectionCamera );
        TextureManager::getSingleton().remove( actor->mReflectionTexture );
        delete actor;

        efficientVectorRemove( mActors, itor );
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::destroyAllActors(void)
    {
        PlanarReflectionActorVec::iterator itor = mActors.begin();
        PlanarReflectionActorVec::iterator end  = mActors.end();

        while( itor != end )
        {
            PlanarReflectionActor *actor = *itor;
            mCompositorManager->removeWorkspace( actor->mWorkspace );
            mSceneManager->destroyCamera( actor->mReflectionCamera );
            TextureManager::getSingleton().remove( actor->mReflectionTexture );
            delete actor;
            ++itor;
        }

        mActors.clear();
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::updateFlushedRenderables(void)
    {
        mUpdatingRenderablesHlms = true;

        TrackedRenderableArray::iterator itor = mTrackedRenderables.begin();
        TrackedRenderableArray::iterator end  = mTrackedRenderables.end();

        while( itor != end )
        {
            TrackedRenderable &trackedRenderable = *itor;
            if( hasFlushPending( trackedRenderable.renderable ) )
            {
                HlmsDatablock *datablock = trackedRenderable.renderable->getDatablock();

                try
                {
                    Hlms *hlms = datablock->getCreator();

                    trackedRenderable.hlmsHashes[0] = trackedRenderable.renderable->getHlmsHash();

                    uint32 hash, casterHash;
                    hlms->calculateHashFor( trackedRenderable.renderable, hash, casterHash );
                    trackedRenderable.hlmsHashes[1] = hash;
                }
                catch( Exception &e )
                {
                    trackedRenderable.hlmsHashes[1] = trackedRenderable.hlmsHashes[0];

                    const String *datablockNamePtr = datablock->getNameStr();
                    String datablockName = datablockNamePtr ? *datablockNamePtr :
                                                              datablock->getName().getFriendlyText();

                    LogManager::getSingleton().logMessage( e.getFullDescription() );
                    LogManager::getSingleton().logMessage(
                                "Couldn't apply planar reflections change to datablock '" +
                                datablockName + "' for this renderable. Disabling reflections. "
                                "Check previous log messages to see if there's more information.",
                                LML_CRITICAL );
                }
            }

            ++itor;
        }

        mUpdatingRenderablesHlms = false;
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::addRenderable( const TrackedRenderable &trackedRenderable )
    {
        assert( trackedRenderable.renderable->mCustomParameter == 0 );
        trackedRenderable.renderable->mCustomParameter = FlushPending;
        mTrackedRenderables.push_back( trackedRenderable );
        _notifyRenderableFlushedHlmsDatablock( trackedRenderable.renderable );
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::removeRenderable( Renderable *renderable )
    {
        TrackedRenderableArray::iterator itor = mTrackedRenderables.begin();
        TrackedRenderableArray::iterator end  = mTrackedRenderables.end();

        while( itor != end && itor->renderable != renderable )
            ++itor;

        assert( itor != end && "Item not found or already removed from this PlanarReflection!" );

        if( itor != end )
        {
            itor->renderable->mCustomParameter = 0;
            if( !hasFlushPending( itor->renderable ) )
            {
                //Restore the Hlms setting. If a flush is pending, then it's already up to date,
                //and our TrackedRenderable::hlmsHashes[0] could be out of date.
                itor->renderable->_setHlmsHashes( itor->hlmsHashes[0],
                                                  itor->renderable->getHlmsCasterHash() );
            }
            efficientVectorRemove( mTrackedRenderables, itor );
        }
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::_notifyRenderableFlushedHlmsDatablock( Renderable *renderable )
    {
        if( !mUpdatingRenderablesHlms )
        {
            renderable->mCustomParameter = FlushPending;
            mAnyPendingFlushRenderable = true;
        }
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::beginFrame(void)
    {
        mLastCamera = 0;
        mLastAspectRatio = 0;

        mActiveActors.clear();
    }
    //-----------------------------------------------------------------------------------
    struct OrderPlanarReflectionActorsByDistanceToPoint
    {
        Vector3 point;

        OrderPlanarReflectionActorsByDistanceToPoint( const Vector3 &p ) :
            point( p ) {}

        bool operator () ( PlanarReflectionActor *_l, PlanarReflectionActor *_r ) const
        {
            return _l->getSquaredDistanceTo( point ) < _r->getSquaredDistanceTo( point );
        }
    };
    void PlanarReflections::update( Camera *camera, Real aspectRatio )
    {
        /*if( mLockCamera && camera != mLockCamera )
            return; //This is not the camera we are allowed to work with

        if( mLastCamera == camera &&
            mLastAspectRatio == camera->getAspectRatio() &&
            (!mLockCamera &&
             mLastCameraPos == camera->getDerivedPosition() &&
             mLastCameraRot == camera->getDerivedOrientation()) )
        {
            return;
        }*/

        if( mAnyPendingFlushRenderable )
        {
            updateFlushedRenderables();
            mAnyPendingFlushRenderable = false;
        }

        mActiveActors.clear();

        mLastAspectRatio = camera->getAspectRatio();
        mLastCameraPos = camera->getDerivedPosition();
        mLastCameraRot = camera->getDerivedOrientation();
        mLastCamera = camera;

        struct ArrayPlane
        {
            ArrayVector3    normal;
            ArrayReal       negD;
        };

        const Vector3 camPos( camera->getDerivedPosition() );
        const Quaternion camRot( camera->getDerivedOrientation() );
        Real nearPlane = camera->getNearClipDistance();
        Real farPlane = camera->getFarClipDistance();

        //Update reflection cameras to keep up their data with the master camera.
        PlanarReflectionActorVec::iterator itor = mActors.begin();
        PlanarReflectionActorVec::iterator end  = mActors.end();

        while( itor != end )
        {
            PlanarReflectionActor *actor = *itor;
            actor->mReflectionCamera->setPosition( camPos );
            actor->mReflectionCamera->setOrientation( camRot );
            actor->mReflectionCamera->setNearClipDistance( nearPlane );
            actor->mReflectionCamera->setFarClipDistance( farPlane );
            actor->mReflectionCamera->setAspectRatio( aspectRatio );
            ++itor;
        }

        //Cull actors against their master cameras (under SSE2, we cull 4 actors at the same time).
        //The culling algorithm is very similar to what is done in OgreForwardClustered.cpp which
        //is also described in
        //www.yosoygames.com.ar/wp/2016/12/frustum-vs-pyramid-intersection-also-frustum-vs-frustum/
        //However the difference is that it's like doing frustum vs frustum test; but the Actor
        //is a frustum with 0 height (thus flattened, hence... a plane) and we also have the
        //guarantee that the opposing sides normals are the same but negated; thus:
        //  rightPlane->normal = -leftPlane->normal
        //  farPlane->normal = -nearPlane->normal
        //Which is something a regular frustum won't guarantee. This saves us space storage.
        const size_t numActors = mActors.size();

        Camera *prevCameras[ARRAY_PACKED_REALS];
        memset( prevCameras, 0, sizeof( prevCameras ) );

        ArrayPlane frustums[6];
        ArrayVector3 worldSpaceCorners[8];

        ArrayActorPlane * RESTRICT_ALIAS actorsPlanes = mActorsSoA;

        {
            const Vector3 *corners = camera->getWorldSpaceCorners();
            for( int i=0; i<8; ++i )
                worldSpaceCorners[i].setAll( corners[i] );

            const Plane *planes = camera->getFrustumPlanes();
            for( int i=0; i<6; ++i )
            {
                frustums[i].normal.setAll( planes[i].normal );
                frustums[i].negD = Mathlib::SetAll( -planes[i].d );
            }
        }

        for( size_t i=0; i<numActors; i += ARRAY_PACKED_REALS )
        {
            ArrayMaskR mask;
            mask = BooleanMask4::getAllSetMask();

            //Test all 4 quad vertices against each of the 6 frustum planes.
            for( int k=0; k<6; ++k )
            {
                ArrayMaskR vertexMask = ARRAY_MASK_ZERO;
                ArrayReal dotResult;

                ArrayVector3 tangentDir, vertexPoint;

                tangentDir = actorsPlanes->planeNormals.yAxis() * actorsPlanes->xyHalfSize[1];
                vertexPoint = actorsPlanes->center + tangentDir;
                dotResult = frustums[k].normal.dotProduct( vertexPoint ) - frustums[k].negD;
                vertexMask = Mathlib::Or( vertexMask,
                                          Mathlib::CompareGreater( dotResult,
                                                                   ARRAY_REAL_ZERO ) );

                vertexPoint = actorsPlanes->center - tangentDir;
                dotResult = frustums[k].normal.dotProduct( vertexPoint ) - frustums[k].negD;
                vertexMask = Mathlib::Or( vertexMask,
                                          Mathlib::CompareGreater( dotResult,
                                                                   ARRAY_REAL_ZERO ) );

                tangentDir = actorsPlanes->planeNormals.xAxis() * actorsPlanes->xyHalfSize[0];
                vertexPoint = actorsPlanes->center + tangentDir;
                dotResult = frustums[k].normal.dotProduct( vertexPoint ) - frustums[k].negD;
                vertexMask = Mathlib::Or( vertexMask,
                                          Mathlib::CompareGreater( dotResult,
                                                                   ARRAY_REAL_ZERO ) );

                vertexPoint = actorsPlanes->center - tangentDir;
                dotResult = frustums[k].normal.dotProduct( vertexPoint ) - frustums[k].negD;
                vertexMask = Mathlib::Or( vertexMask,
                                          Mathlib::CompareGreater( dotResult,
                                                                   ARRAY_REAL_ZERO ) );

                mask = Mathlib::And( mask, vertexMask );
            }

            if( BooleanMask4::getScalarMask( mask ) != 0 )
            {
                //Test all 8 frustum corners against each of the 6 planes (5+1).
                ArrayVector3 actorPlaneNormal;
                ArrayReal actorPlaneNegD;
                ArrayReal dotResult;
                ArrayMaskR vertexMask;

                //Main plane (positive side)
                actorPlaneNormal = actorsPlanes->planeNormals.zAxis();
                actorPlaneNegD = actorsPlanes->planeNegD[0];
                vertexMask = ARRAY_MASK_ZERO;
                //Only test against the vertices from the near frustum. If they're all
                //in the negative side of this plane, then this plane is looking
                //away from the camera
                //for( int l=0; l<8; ++l )
                for( int l=0; l<4; ++l )
                {
                    dotResult = actorPlaneNormal.dotProduct( worldSpaceCorners[l] ) - actorPlaneNegD;
                    vertexMask = Mathlib::Or( vertexMask,
                                              Mathlib::CompareGreater( dotResult,
                                                                       ARRAY_REAL_ZERO ) );
                }
                mask = Mathlib::And( mask, vertexMask );

                //Main plane (negative side)
                actorPlaneNormal = -actorPlaneNormal;
                actorPlaneNegD = -actorsPlanes->planeNegD[0];
                vertexMask = ARRAY_MASK_ZERO;
                for( int l=0; l<8; ++l )
                {
                    dotResult = actorPlaneNormal.dotProduct( worldSpaceCorners[l] ) - actorPlaneNegD;
                    vertexMask = Mathlib::Or( vertexMask,
                                              Mathlib::CompareGreater( dotResult,
                                                                       ARRAY_REAL_ZERO ) );
                }
                mask = Mathlib::And( mask, vertexMask );

                //North plane
                actorPlaneNormal = actorsPlanes->planeNormals.yAxis();
                actorPlaneNegD = actorsPlanes->planeNegD[1];
                vertexMask = ARRAY_MASK_ZERO;
                for( int l=0; l<8; ++l )
                {
                    dotResult = actorPlaneNormal.dotProduct( worldSpaceCorners[l] ) - actorPlaneNegD;
                    vertexMask = Mathlib::Or( vertexMask,
                                              Mathlib::CompareGreater( dotResult,
                                                                       ARRAY_REAL_ZERO ) );
                }
                mask = Mathlib::And( mask, vertexMask );

                //South plane
                actorPlaneNormal = -actorPlaneNormal;
                actorPlaneNegD = actorsPlanes->planeNegD[2];
                vertexMask = ARRAY_MASK_ZERO;
                for( int l=0; l<8; ++l )
                {
                    dotResult = actorPlaneNormal.dotProduct( worldSpaceCorners[l] ) - actorPlaneNegD;
                    vertexMask = Mathlib::Or( vertexMask,
                                              Mathlib::CompareGreater( dotResult,
                                                                       ARRAY_REAL_ZERO ) );
                }
                mask = Mathlib::And( mask, vertexMask );

                //East plane
                actorPlaneNormal = actorsPlanes->planeNormals.xAxis();
                actorPlaneNegD = actorsPlanes->planeNegD[3];
                vertexMask = ARRAY_MASK_ZERO;
                for( int l=0; l<8; ++l )
                {
                    dotResult = actorPlaneNormal.dotProduct( worldSpaceCorners[l] ) - actorPlaneNegD;
                    vertexMask = Mathlib::Or( vertexMask,
                                              Mathlib::CompareGreater( dotResult,
                                                                       ARRAY_REAL_ZERO ) );
                }
                mask = Mathlib::And( mask, vertexMask );

                //West plane
                actorPlaneNormal = -actorPlaneNormal;
                actorPlaneNegD = actorsPlanes->planeNegD[4];
                vertexMask = ARRAY_MASK_ZERO;
                for( int l=0; l<8; ++l )
                {
                    dotResult = actorPlaneNormal.dotProduct( worldSpaceCorners[l] ) - actorPlaneNegD;
                    vertexMask = Mathlib::Or( vertexMask,
                                              Mathlib::CompareGreater( dotResult,
                                                                       ARRAY_REAL_ZERO ) );
                }
                mask = Mathlib::And( mask, vertexMask );
            }

            const uint32 scalarMask = BooleanMask4::getScalarMask( mask );

            for( size_t j=0; j<ARRAY_PACKED_REALS; ++j )
            {
                if( i + j < mActors.size() )
                {
                    if( IS_BIT_SET( j, scalarMask ) )
                        mActiveActors.push_back( mActors[i + j] );
                    else
                        mActors[i + j]->mWorkspace->setEnabled( false );
                }
            }

            ++actorsPlanes;
        }

        std::sort( mActiveActors.begin(), mActiveActors.end(),
                   OrderPlanarReflectionActorsByDistanceToPoint( camPos ) );

        itor = mActiveActors.begin();
        end  = mActiveActors.end();

        while( itor != end )
        {
            const size_t idx = itor - mActiveActors.begin();
            if( idx < mMaxActiveActors )
                (*itor)->mWorkspace->setEnabled( true );
            else
                (*itor)->mWorkspace->setEnabled( false );
            ++itor;
        }

        mActiveActors.resize( std::min<size_t>( mActiveActors.size(), mMaxActiveActors ) );

        TrackedRenderableArray::const_iterator itTracked = mTrackedRenderables.begin();
        TrackedRenderableArray::const_iterator enTracked = mTrackedRenderables.end();

        while( itTracked != enTracked )
        {
            if( itTracked->movableObject->getVisible() )
            {
                //const Matrix4 &fullTransform = itTracked->movableObject->_getParentNodeFullTransform();
                //TODO
                const Matrix4 &fullTransform = itTracked->movableObject->
                        getParentNode()->_getFullTransformUpdated();
                Matrix3 rotMat3x3;
                fullTransform.extract3x3Matrix( rotMat3x3 );
                Vector3 reflNormal = rotMat3x3 * itTracked->reflNormal;
                const Vector3 rendCenter = fullTransform * itTracked->renderableCenter;

                //Undo any scale
                reflNormal.normalise();

                uint8 bestActorIdx = mMaxActiveActors;
                Real bestCosAngle = -1;
                Real bestSqDistance = std::numeric_limits<Real>::max();

                itor = mActiveActors.begin();
                end  = mActiveActors.end();

                while( itor != end )
                {
                    PlanarReflectionActor *actor = *itor;
                    const Real cosAngle = actor->getNormal().dotProduct( reflNormal );

                    const Real cos20 = 0.939692621f;

                    if( cosAngle >= cos20 &&
                        (cosAngle >= bestCosAngle ||
                        Math::Abs(cosAngle - bestCosAngle) < Real( 0.060307379f )) )
                    {
                        Real sqDistance = actor->getSquaredDistanceTo( rendCenter );
                        if( sqDistance < mMaxSqDistance && sqDistance <= bestSqDistance )
                        {
                            bestActorIdx = static_cast<uint8>( itor - mActiveActors.begin() );
                        }
                    }

                    ++itor;
                }

                if( bestActorIdx < mMaxActiveActors )
                {
                    itTracked->renderable->mCustomParameter = (UseActiveActor | bestActorIdx);
                    itTracked->renderable->_setHlmsHashes( itTracked->hlmsHashes[1],
                                                           itTracked->renderable->getHlmsCasterHash() );
                }
                else
                {
                    itTracked->renderable->mCustomParameter = InactiveActor;
                    itTracked->renderable->_setHlmsHashes( itTracked->hlmsHashes[0],
                                                           itTracked->renderable->getHlmsCasterHash() );
                }
            }

            ++itTracked;
        }
    }
    //-----------------------------------------------------------------------------------
    size_t PlanarReflections::getConstBufferSize(void) const
    {
        return (4u * mMaxActiveActors + 4u * 4u + 4u) * sizeof(float);
    }
    //-----------------------------------------------------------------------------------
    void PlanarReflections::fillConstBufferData( RenderTarget *renderTarget, Camera *camera,
                                                 const Matrix4 &projectionMatrix,
                                                 float * RESTRICT_ALIAS passBufferPtr ) const
    {
        const Matrix4 viewMatrix = camera->getViewMatrix( true );
        Matrix3 viewMat3x3;
        viewMatrix.extract3x3Matrix( viewMat3x3 );

        PlanarReflectionActorVec::const_iterator itor = mActiveActors.begin();
        PlanarReflectionActorVec::const_iterator end  = mActiveActors.end();

        while( itor != end )
        {
            const Plane &plane = (*itor)->mPlane;

            //We need to send the plane data for PBS (which will
            //be processed in pixel shader)in view space.
            const Vector3 viewSpacePointInPlane = viewMatrix * (plane.normal * -plane.d);
            const Vector3 viewSpaceNormal       = viewMat3x3 * plane.normal;

            Plane planeVS( viewSpaceNormal, viewSpacePointInPlane );

            *passBufferPtr++ = static_cast<float>( planeVS.normal.x );
            *passBufferPtr++ = static_cast<float>( planeVS.normal.y );
            *passBufferPtr++ = static_cast<float>( planeVS.normal.z );
            *passBufferPtr++ = static_cast<float>( planeVS.d );

            ++itor;
        }

        memset( passBufferPtr, 0, (mMaxActiveActors - mActiveActors.size()) * 4u * sizeof(float) );
        passBufferPtr += (mMaxActiveActors - mActiveActors.size()) * 4u;

        Matrix4 reflProjMat = PROJECTIONCLIPSPACE2DTOIMAGESPACE_PERSPECTIVE * projectionMatrix;
        for( size_t i=0; i<16; ++i )
            *passBufferPtr++ = (float)reflProjMat[0][i];

        *passBufferPtr++ = static_cast<float>( mInvMaxDistance );
        *passBufferPtr++ = 1.0f;
        *passBufferPtr++ = 1.0f;
        *passBufferPtr++ = 1.0f;
    }
    //-----------------------------------------------------------------------------------
    TexturePtr PlanarReflections::getTexture( uint8 actorIdx ) const
    {
        if( actorIdx >= mActiveActors.size() )
            return TexturePtr();
        return mActiveActors[actorIdx]->mReflectionTexture;
    }
    //-----------------------------------------------------------------------------------
    bool PlanarReflections::cameraMatches( const Camera *camera )
    {
        return  !camera->isReflected() &&
                mLastAspectRatio == camera->getAspectRatio() &&
                mLastCameraPos == camera->getDerivedPosition() &&
                mLastCameraRot == camera->getDerivedOrientation();
    }
    //-----------------------------------------------------------------------------------
    bool PlanarReflections::_isUpdatingRenderablesHlms(void) const
    {
        return mUpdatingRenderablesHlms;
    }
    //-----------------------------------------------------------------------------------
    bool PlanarReflections::hasPlanarReflections( const Renderable *renderable ) const
    {
        return  renderable->mCustomParameter & UseActiveActor ||
                renderable->mCustomParameter == FlushPending ||
                renderable->mCustomParameter == InactiveActor;
    }
    //-----------------------------------------------------------------------------------
    bool PlanarReflections::hasFlushPending( const Renderable *renderable ) const
    {
        return renderable->mCustomParameter == FlushPending;
    }
    //-----------------------------------------------------------------------------------
    bool PlanarReflections::hasActiveActor( const Renderable *renderable ) const
    {
        return (renderable->mCustomParameter & UseActiveActor) != 0;
    }
}