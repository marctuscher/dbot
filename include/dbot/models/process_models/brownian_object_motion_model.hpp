/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014 Max-Planck-Institute for Intelligent Systems,
 *                     University of Southern California
 *    Jan Issac (jan.issac@gmail.com)
 *    Manuel Wuthrich (manuel.wuthrich@gmail.com)
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * @date 05/25/2014
 * @author Jan Issac (jan.issac@gmail.com)
 * @author Manuel Wuthrich (manuel.wuthrich@gmail.com)
 * Max-Planck-Institute for Intelligent Systems, University of Southern California
 */


#ifndef POSE_TRACKING_MODELS_PROCESS_MODELS_BROWNIAN_OBJECT_MOTION_MODEL_HPP
#define POSE_TRACKING_MODELS_PROCESS_MODELS_BROWNIAN_OBJECT_MOTION_MODEL_HPP

#include <fl/util/math.hpp>
#include <fl/util/assertions.hpp>
#include <fl/model/process/integrated_damped_wiener_process_model.hpp>

#include <dbot/states/free_floating_rigid_bodies_state.hpp>
#include <fl/model/process/damped_wiener_process_model.hpp>

namespace fl
{

// Forward declarations
template <typename State, int OBJECTS> class BrownianObjectMotionModel;

/**
 * BrownianObjectMotion distribution traits specialization
 * \internal
 */
template <typename State_, int OBJECTS>
struct Traits<BrownianObjectMotionModel<State_, OBJECTS> >
{
    enum
    {
        DIMENSION_PER_OBJECT = 6,
        INPUT_DIMENSION = (OBJECTS == -1) ? -1 : OBJECTS * DIMENSION_PER_OBJECT
    };

    typedef State_                                      State;
    typedef typename State::Scalar                      Scalar;
    typedef Eigen::Matrix<Scalar, INPUT_DIMENSION, 1>   Input;
    typedef Input Noise;

    typedef Eigen::Quaternion<Scalar>                      Quaternion;
    typedef Eigen::Matrix<Scalar, DIMENSION_PER_OBJECT, 1> ObjectState;
    typedef IntegratedDampedWienerProcessModel<ObjectState>     Process;

    typedef StandardGaussianMapping<State, Noise>          GaussianMappingBase;
    typedef ProcessModelInterface<State, Noise, Input> ProcessInterfaceBase;
};

/**
 * \class BrownianObjectMotion
 *
 * \ingroup distributions
 * \ingroup process_models
 */
template <typename State_, int OBJECTS = -1>
class BrownianObjectMotionModel
        : public Traits<BrownianObjectMotionModel<State_, OBJECTS>>::GaussianMappingBase,
          public Traits<BrownianObjectMotionModel<State_, OBJECTS>>::ProcessInterfaceBase
{
public:

    typedef BrownianObjectMotionModel<State_, OBJECTS> This;

    typedef typename Traits<This>::Scalar     Scalar;
    typedef typename Traits<This>::State      State;
    typedef typename Traits<This>::Input      Input;
    typedef typename Traits<This>::Noise      Noise;
    typedef typename Traits<This>::Quaternion Quaternion;
    typedef typename Traits<This>::Process    Process;

    enum
    {
        DIMENSION_PER_OBJECT = Traits<This>::DIMENSION_PER_OBJECT
    };

public:
    BrownianObjectMotionModel(const unsigned& count_objects = OBJECTS):
        Traits<This>::GaussianMappingBase(
            count_objects == Eigen::Dynamic ? Eigen::Dynamic : count_objects * DIMENSION_PER_OBJECT),
        state_(count_objects)
    {
        static_assert_base(State, FreeFloatingRigidBodiesState<OBJECTS>);

        quaternion_map_.resize(count_objects);
        rotation_center_.resize(count_objects);
        linear_process_.resize(count_objects);
        angular_process_.resize(count_objects);
    }

    virtual ~BrownianObjectMotionModel() { }

    virtual State map_standard_normal(const Noise& sample) const
    {
        State new_state(state_.body_count());
        for(size_t i = 0; i < new_state.body_count(); i++)
        {
            Eigen::Matrix<Scalar, 3, 1> position_noise    = sample.template middleRows<3>(i*DIMENSION_PER_OBJECT);
            Eigen::Matrix<Scalar, 3, 1> orientation_noise = sample.template middleRows<3>(i*DIMENSION_PER_OBJECT + 3);
            Eigen::Matrix<Scalar, 6, 1> linear_delta      = linear_process_[i].map_standard_normal(position_noise);
            Eigen::Matrix<Scalar, 6, 1> angular_delta     = angular_process_[i].map_standard_normal(orientation_noise);

            new_state.position(i) = state_.position(i) + linear_delta.topRows(3);
            Quaternion updated_quaternion(state_.quaternion(i).coeffs() + quaternion_map_[i] * angular_delta.topRows(3));
            new_state.quaternion(updated_quaternion.normalized(), i);
            new_state.linear_velocity(i)  = linear_delta.bottomRows(3);
            new_state.angular_velocity(i) = angular_delta.bottomRows(3);

            // transform to external coordinate system
            new_state.linear_velocity(i) -= new_state.angular_velocity(i).cross(state_.position(i));
            new_state.position(i)        -= new_state.rotation_matrix(i)*rotation_center_[i];
        }

        return new_state;
    }

    virtual void condition(const Scalar& delta_time,
                           const State&  state,
                           const Input&  control)
    {
        state_ = state;
        for(size_t i = 0; i < state_.body_count(); i++)
        {
            quaternion_map_[i] = fl::quaternion_matrix(state_.quaternion(i).coeffs());

            // transform the state, which is the pose and velocity with respect to to the origin,
            // into internal representation, which is the position and velocity of the center
            // and the orientation and angular velocity around the center
            state_.position(i)        += state_.rotation_matrix(i)*rotation_center_[i];
            state_.linear_velocity(i) += state_.angular_velocity(i).cross(state_.position(i));

            Eigen::Matrix<Scalar, 6, 1> linear_state;
            linear_state.topRows(3) = Eigen::Vector3d::Zero();
            linear_state.bottomRows(3) = state_.linear_velocity(i);
            linear_process_[i].condition(delta_time,
                                         linear_state,
                                         control.template middleRows<3>(i*DIMENSION_PER_OBJECT));

            Eigen::Matrix<Scalar, 6, 1> angular_state;
            angular_state.topRows(3) = Eigen::Vector3d::Zero();
            angular_state.bottomRows(3) = state_.angular_velocity(i);
            angular_process_[i].condition(delta_time,
                                          angular_state,
                                          control.template middleRows<3>(i*DIMENSION_PER_OBJECT + 3));
        }
    }

    virtual void parameters(const size_t&                           object_index,
                            const Eigen::Matrix<Scalar, 3, 1>&  rotation_center,
                            const Scalar&                       damping,
                            const typename Traits<Process>::SecondMoment&   linear_acceleration_covariance,
                            const typename Traits<Process>::SecondMoment&   angular_acceleration_covariance)
    {
        rotation_center_[object_index] = rotation_center;
        linear_process_[object_index].parameters(damping, linear_acceleration_covariance);
        angular_process_[object_index].parameters(damping, angular_acceleration_covariance);
    }

    virtual unsigned InputDimension() const
    {
        return this->standard_variate_dimension();
    }

    virtual int dimension()
    {
        return state_.rows();
    }

    /* interface API */
    virtual State predict_state(double delta_time,
                                const State& state,
                                const Noise& noise,
                                const Input& input)
    {
        condition(delta_time, state, input);

        return map_standard_normal(noise);
    }

    virtual int state_dimension() const
    {
        return state_.rows();
    }

    virtual int noise_dimension() const
    {
        return this->standard_variate_dimension();
    }

    virtual int input_dimension() const
    {
        return this->standard_variate_dimension();
    }

private:
    // conditionals
    State state_;
    std::vector<Eigen::Matrix<Scalar, 4, 3> > quaternion_map_;

    // parameters
    std::vector<Eigen::Matrix<Scalar, 3, 1> > rotation_center_;

    // processes
    std::vector<Process>   linear_process_;
    std::vector<Process>   angular_process_;
};

}

#endif