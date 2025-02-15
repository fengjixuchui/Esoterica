#include "PlayerAction_Jump.h"
#include "Game/Player/Components/Component_MainPlayer.h"
#include "Game/Player/Physics/PlayerPhysicsController.h"
#include "Game/Player/Camera/PlayerCameraController.h"
#include "Game/Player/Animation/PlayerAnimationController.h"
#include "Game/Player/Animation/PlayerGraphController_Ability.h"
#include "Engine/Physics/Components/Component_PhysicsCharacter.h"
#include "System/Input/InputSystem.h"

// hack for now
#include "Game/Player/Animation/PlayerGraphController_Locomotion.h"

//-------------------------------------------------------------------------

namespace EE::Player
{
    static Radians  g_maxAngularSpeed = Radians( Degrees( 90 ) ); // radians/second
    static float    g_smallJumpDistance = 3.0f;                   // meters/second
    static float    g_bigJumpDistance = 8.0f;                     // meters/second
    static float    g_bigJumpEnergyCost = 1.0f;                   // energy levels
    static float    g_gravityAcceleration = 30.0f;                // meters/second squared
    static float    g_maxAirControlAcceleration = 10.0f;          // meters/second squared
    static float    g_maxAirControlSpeed = 6.5f;                  // meters/second
    static Seconds  g_bigJumpHoldTime = 0.3f;                     // seconds

    //  1.) V = Vi + a(t)
    //
    //      0 = Vi + a(t)                   V = 0 since we want to reach the apex, hence velocity 0.
    //      Vi = -a(t)
    //
    //  2.)	d = Vi(t) + 0.5(a)(t^2)
    //
    //      d = -a(t)(t) + 0.5(a)(t^2)      substitute vi = -a(t) from 1.
    //      d = -a(t^2)  + 0.5(a)(t^2)
    //      d =  a(t^2)(-1 + 0.5)
    //      d = -0.5(a)(t^2)
    //      t^2 = -2(d)/a
    //      t = sqrt( -2(d)/a )
    //  
    //      Vi = -a(t)                      back to using 1. now that we have t we can calculate Vi.
    //      Vi = -a( sqrt( -2(d)/a ) )      the negative sign will disappear since our acceleration is negative

    static float    g_bigJumpTimeToApex = Math::Sqrt( 2 * g_bigJumpDistance / ( g_gravityAcceleration ) );
    static float    g_bigJumpinitialSpeed = g_gravityAcceleration * g_bigJumpTimeToApex;

    static float    g_smallJumpTimeToApex = Math::Sqrt( 2 * g_smallJumpDistance / ( g_gravityAcceleration ) );
    static float    g_smallJumpinitialSpeed = g_gravityAcceleration * g_smallJumpTimeToApex;

    //-------------------------------------------------------------------------

    bool JumpAction::TryStartInternal( ActionContext const& ctx )
    {
        if( ctx.m_pInputState->GetControllerState()->WasReleased( Input::ControllerButton::FaceButtonDown ) )
        {
            ctx.m_pAnimationController->SetCharacterState( CharacterAnimationState::Ability );
            auto pAbilityAnimController = ctx.GetAnimSubGraphController<AbilityGraphController>();
            pAbilityAnimController->StartJump();

            ctx.m_pCharacterController->DisableGravity();
            ctx.m_pCharacterController->DisableProjectionOntoFloor();
            ctx.m_pCharacterController->EnableStepHeight();
            m_jumpTimer.Start();

            if( m_isChargedJumpReady )
            {
                ctx.m_pPlayerComponent->ConsumeEnergy( g_bigJumpEnergyCost );
            }
            m_previousHeight = 0.0f;

            return true;
        }
        else // Check hold state
        {
            m_isChargedJumpReady = false;
            Seconds jumpHoldTime = 0.0f;
            if( ctx.m_pInputState->GetControllerState()->IsHeldDown( Input::ControllerButton::FaceButtonDown, &jumpHoldTime ) )
            {
                if( jumpHoldTime > g_bigJumpHoldTime && ctx.m_pPlayerComponent->HasEnoughEnergy( g_bigJumpEnergyCost ) )
                {
                    m_isChargedJumpReady = true;
                }
            }
        }

        return false;
    }

    Action::Status JumpAction::UpdateInternal( ActionContext const& ctx )
    {
        // check if we had a collision
        //-------------------------------------------------------------------------
        float const jumpTime = ( m_isChargedJumpReady ? g_bigJumpTimeToApex : g_smallJumpTimeToApex );
        if( m_jumpTimer.GetElapsedTimeSeconds() >= jumpTime )
        {
            // Jump Completed
            return Status::Completed;
        }
        else if( m_jumpTimer.GetElapsedTimeSeconds() > 0.0f && ctx.m_pCharacterComponent->GetCharacterVelocity().m_z <= 0.0f )
        {
            // Jump Collided with a over head collision
            return Status::Completed;
        }
        else
        {
            m_jumpTimer.Update( ctx.GetDeltaTime() );

            float deltaHeight = ctx.m_pPlayerComponent->m_jumpCurve.Evaluate( m_jumpTimer.GetElapsedTimeSeconds() / jumpTime ) * ( m_isChargedJumpReady ? g_bigJumpDistance : g_smallJumpDistance ) - m_previousHeight;
            float verticalVelocity = deltaHeight / ctx.GetDeltaTime();
            m_previousHeight += deltaHeight;

            auto const pControllerState = ctx.m_pInputState->GetControllerState();
            EE_ASSERT( pControllerState != nullptr );

            // Calculate desired player displacement
            //-------------------------------------------------------------------------
            Vector const movementInputs = pControllerState->GetLeftAnalogStickValue();
            auto const& camFwd = ctx.m_pCameraController->GetCameraRelativeForwardVector2D();
            auto const& camRight = ctx.m_pCameraController->GetCameraRelativeRightVector2D();

            // Use last frame camera orientation
            Vector const CurrentVelocity = ctx.m_pCharacterComponent->GetCharacterVelocity();
            Vector const CurrentVelocity2D = CurrentVelocity * Vector( 1.0f, 1.0f, 0.0f );
            float const CurrentSpeed2D = CurrentVelocity2D.GetLength2();

            Vector const forward = camFwd * movementInputs.m_y;
            Vector const right = camRight * movementInputs.m_x;
            Vector const desiredHeadingVelocity2D = ( forward + right ) * g_maxAirControlAcceleration * ctx.GetDeltaTime();

            Vector ResultingVelocity = CurrentVelocity2D + desiredHeadingVelocity2D;
            float const Length = ResultingVelocity.GetLength2();
            if( Length > g_maxAirControlSpeed )
            {
                ResultingVelocity = ResultingVelocity.GetNormalized2() * g_maxAirControlSpeed;
            }
            ResultingVelocity.m_z = verticalVelocity;

            Vector const Facing = desiredHeadingVelocity2D.IsZero2() ? ctx.m_pCharacterComponent->GetForwardVector() : desiredHeadingVelocity2D.GetNormalized2();

            // Update animation controller
            //-------------------------------------------------------------------------
            auto pLocomotionGraphController = ctx.GetAnimSubGraphController<LocomotionGraphController>();
            pLocomotionGraphController->SetLocomotionDesires( ctx.GetDeltaTime(), ResultingVelocity, Facing );
        }

        return Status::Interruptible;
    }

    void JumpAction::StopInternal( ActionContext const& ctx, StopReason reason )
    {

    }
}