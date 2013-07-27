/**\file MotionModel.hpp
 *
 * This clase provided a Motion Model solver for any mobile robot (wheel or leg)
 * with articulated joints.
 *
 * The solver is based on Weigthed Least-Squares as minimizing the error to estimate the
 * resulting motion from a robot Jacobian. Robot jacobian is understood as the sparse Jacobian
 * matrix containing one jacobian per each Tree of the Robot.
 * Therefore, it is a minimizing problem of a non-linear system which has been previously linearized
 * around the working point (robot current joint position values). This is represented in the
 * current Jacobian matrix of the robot.
 * This class uses the KinematicModel abtsract class (in this library) to access the robot Jacobian
 * and compute the statistical motion model (estimated velocities/navigation quantities).
 *
 * Further Details at:  P.Muir et. al Kinematic Modeling of Wheeled Mobile Robots
 *                      M. Tarokh et. al Kinematics Modeling and Analyses of Articulated Rovers
 *                      J. Hidalgo et. al Kinematics Modeling of a Hybrid Wheeled-Leg Planetary Rover
 *                      J. Hidalgo, Navigation and Slip Kinematics for High Performance Motion Models
 *
 * @author Javier Hidalgo Carrio | DFKI RIC Bremen | javier.hidalgo_carrio@dfki.de
 * @date June 2013.
 * @version 1.0.
 */


#ifndef ODOMETRY_MOTION_MODEL_HPP
#define ODOMETRY_MOTION_MODEL_HPP

#include <iostream> /** for std::cout TO-DO REMOVE **/
#include <vector> /** For std vector **/
#include <boost/shared_ptr.hpp> /** For std shared pointer **/
#include <base/logging.h> /** Log meassges **/
#include <Eigen/Geometry> /** Eigen data type for Matrix, Quaternion, etc... */
#include <Eigen/Core> /** Core methods of Eigen implementation **/
#include <Eigen/Dense> /** for the algebra and transformation matrices **/
#include <Eigen/Cholesky> /** For the Cholesky decomposition **/
#include <Eigen/Dense> /** For accessing Matrixclock and corner amomg others**/
#include "KinematicModel.hpp"


//#define DEBUG_PRINTS_ODOMETRY_MOTION_MODEL 1 //TO-DO: Remove this. Only for testing (master branch) purpose

namespace odometry
{
    /** A struct
     * Struct for the contact points
     */
    struct TreeContactPoint
    {
        /** Number of contact points of the tree. It cannot be negative, zero if the Tree does not have contact **/
        unsigned int number;

        /** Point in contact between 0 and (number-1) or NO_CONTACT if the
         * case of this tree does not have contact **/
        int contactId;

        TreeContactPoint()
            : number(1), contactId(0) {}
    };

    /**@class MotionModel
     *
     * Motion Model solver class
     *
     * @param _Scalar: is the typename of your desired implementation (float, double, etc..)
     * @param _RobotTrees: is the number of independent Trees connected to your desired Body Center.
     *              Trees is understood as connection of kinematics chains. As an example:
     *              <a href="http://robotik.dfki-bremen.de/en/forschung/robotersysteme/asguard-ii.html">Asguard</a>
     *              hybrid wheels model as Trees. Therefore Asguard has 4 Trees. Each Tree has 5 open
     *              kinematics chains, one per each foot which are potential points in contact with
     *              the ground.
     * @param _RobotJointDoF: Complete number of DoF in the Joint Space of your robot.This is every joint
     *              passive or active that your robot (or the part of your robot related to odometry)
     *              has. This is required for the Jacobian in a general form. Therefore, this would
     *              be part of the number of columns in the Kinematic Model Jacobian.
     * @param _SlipDoF: The number of DoF than you want to model the slip of the contact point. If you
     *           are not interested to model slip velocity/displacement set it to zero. Otherwise,
     *           a common slip model has 3DoF (in X an Y direction and Z rotation)
     * @param _ContactDoF: This is the angle of contact between the ground and the contact point. If this
     *              is not interesting for you model (i.e: the robot moves in indoor environment)
     *              set it to zero. Otherwise, for navigation on uneven terrains, it is normally modeled as 1DoF along the
     *              axis of the pitch angle of your robot.
     *
     * Note that it is very important how you especify the number of Trees according to the contact points.
     * It is assumed that one Tree can only have one single point of contact with the ground at the same time.
     * Therefore, if your real/physical kinematic Tree can have more than one contact point at a time (e.g: two)
     * the Tree needs to be split according to it (e.g: two Trees).
     * Take <a href="http://robotik.dfki-bremen.de/en/forschung/robotersysteme/asguard-ii.html">Asguard</a> wheel
     * as an example. If we want to model the wheel as two feet can have point in contact, two Trees
     * needs to be created in the wheel (virtualy increasing the number of Trees).
     *
     */
    template <typename _Scalar, int _RobotTrees, int _RobotJointDoF, int _SlipDoF, int _ContactDoF>
    class MotionModel
    {
        public:
            static const unsigned int MAX_CHAIN_DOF = _RobotJointDoF+_SlipDoF+_ContactDoF;

            /** This is also the order of storage of the values. Columns of the robot Jacobian (J) */
            static const unsigned int MODEL_DOF = _RobotJointDoF+_RobotTrees*(_SlipDoF+_ContactDoF);

            /** The potentical contact point of the kineatic chain  does not have contact **/
            static const int NO_CONTACT = -1;

        public:
            /** Pointer to the kinematic model **/
            typedef boost::shared_ptr< KinematicModel <_Scalar, _RobotTrees, _RobotJointDoF, _SlipDoF, _ContactDoF> > kinematics_ptr;

            /** An Enum
             * Store the different algorithm to find the point which are in contact.
             * In the case of having external means to define the point in contact with the ground
             * (i.e: map represenation) this is not very important.
             * If the point in contact is computed only using the current kinematic configuration
             * of the robot, then it is important.
             */
            enum methodContactPoint
            {
                LOWEST_POINT  = 0,
                COMBINATORICS  = 1
            };

            methodContactPoint contactSelection; /**< Method to select the contact points */
            kinematics_ptr robotModel; /**< Kinematic model of a robot */
            std::vector<Eigen::Affine3d> fkRobot; /**< Forward kinematics of the robot chains */
            std::vector<base::Matrix6d> fkCov; /**< Uncertainty of the forward kinematics (if any) */
            std::vector<TreeContactPoint> contactPoints; /**< Number and point in contact per each tree of the model */

        protected:

            /* This function is deprecated
            bool contactPointComparison(Eigen::Affine3d fkRobot1, Eigen::Affine3d fkRobot2)
            {
                return fkRobot1.translation()[2] < fkRobot2.translation()[2];
            }*/

            /**@brief computes the current set of points in contact choosing the lowest point.
             *
             * This function uses the lowest point w.r.t the local Z axis to compute
             * the points in contact for the robot. One point per Tree.
             * It stores the result in the contact point member of the class.
             *
             * @return void
             */
            void lowestPointInContact()
            {
                register int j = 0;

                /** For all the trees of the model **/
                for (std::vector<TreeContactPoint>::iterator it = contactPoints.begin() ; it != contactPoints.end(); it++)
                {
                    /** More than one contact point per this tree **/
                    if ((*it).number > 1)
                    {
                        double zdistance = 0.00;
                        for (register unsigned int i=0; i<(*it).number; ++i)
                        {
                            if (zdistance > fkRobot[j+i].translation()[2])
                            {
                                zdistance = fkRobot[j+i].translation()[2];
                                (*it).contactId = i;
                            }
                        }
                    }
                    else if ((*it).number == 1) /** If there is only one contact point then it is that one making contact **/
                    {
                        contactPoints[j].contactId = 0;
                    }
                    else /** This tree does not have contact **/
                    {
                        contactPoints[j].contactId = NO_CONTACT;
                    }

                    j = j+(*it).number;
                }

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<<"[MOTION_MODEL] Selected Points in Contact:";
                for (std::vector<TreeContactPoint>::iterator it = contactPoints.begin() ; it != contactPoints.end(); it++)
                {
                    std::cout<<" "<<(*it).contactId;
                }
                std::cout<<"\n";
                #endif


                return;
            }

            /**@brief Computes th epoints in contact using combinatorics
             *
             * Probabilitistic combinatoric method to compute the points in
             * contact for the robot. One point in contact per Tree.
             *
             * TO-DO
             *
             * @return void
             */
            void combinatoricsPointInContact()
            {
                return;
            }

            /**@brief this method computes the point in contact depending on the method.
             *
             * @param method enum variable with the desired method to compute the points in contact
             *
             */
            void selectPointsInContact(MotionModel::methodContactPoint method)
            {

                /** Select the method for the points in contact **/
                if (method == LOWEST_POINT)
                {
                    #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                    std::cout<< "[MOTION_MODEL SelectPoints] Lowest Points method\n";
                    #endif
                    this->lowestPointInContact();
                }
                else if (method == COMBINATORICS)
                {
                    #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                    std::cout<< "[MOTION_MODEL SelectPoints] Combinatorics Points method\n";
                    #endif

                    this->combinatoricsPointInContact();
                }

               return;
            }

            /**@brief Forms the Navigation Equations for the navigation kinematics
             *
             * It assumes that known quantities are angular rotations (cartesian) and joint velocities.
             * non-slip in the X and Y axis. Only slip in z-axis (non-holonomic constrain)
             * The unknow quatities are position of the robot in cartesian, the contact angle and the
             * z value of the slip vector.
             * The system of equations is unknownA * unknownx = knownB * knowny
             * The parameters are stored in the general dimension for the problem, therefore unknow quatities
             * are set to NaN in the parameters of the method.
             *
             * @param[in] cartesianVelocities Eigen vector with the cartesian velocities (w.r.t local body frame).
             * @param[in] modelVelocities Eigen vector with the velocities of the model(joint, slip
             *            and contact angle)
             * @param[in] J robot jacobian matrix.
             * @param[in] cartesianVelCov uncertainty in the measurement of the cartesianVelocities vector.
             * @param[in] modelVelCov uncertainty in the measurement of the modelVelocities vector.
             * @param[out] unknownA is the matrix of unknow quantities.
             * @param[out] unknownx in the vector of unknow quantities.
             * @param[out] knownB is the matrix of know quantities
             * @param[out] knowny is the matrix of know quantities
             * @param[out] Weight matrix with the inverse of the noise for the equation (Weigthed Least-Squares).
             */
            inline void navEquations(const Eigen::Matrix <_Scalar, 6, 1> &cartesianVelocities,
                                    const Eigen::Matrix <_Scalar, MODEL_DOF, 1> &modelVelocities,
                                    const Eigen::Matrix <_Scalar, 6*_RobotTrees, MODEL_DOF> &J,
                                    const Eigen::Matrix <_Scalar, 6, 6> &cartesianVelCov,
                                    const Eigen::Matrix <_Scalar, MODEL_DOF, MODEL_DOF> &modelVelCov,
                                    Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotTrees+(_RobotTrees*_ContactDoF)> &unknownA,
                                    Eigen::Matrix <_Scalar, 3+_RobotTrees+(_RobotTrees*_ContactDoF), 1> &unknownx,
                                    Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotJointDoF> &knownB,
                                    Eigen::Matrix <_Scalar, 3+_RobotJointDoF, 1> &knowny,
                                    Eigen::Matrix <_Scalar, 6*_RobotTrees, 6*_RobotTrees> &Weight)
            {
                Eigen::Matrix<_Scalar, 6*_RobotTrees, 6> spareI;

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<<"[MOTION_MODEL] navEquations\n";
                #endif

                /** Compute the composite rover equation matrix I **/
                for (register int i=0; i<_RobotTrees; ++i)
                    spareI.template block<6, 6>(i*6, 0) = Eigen::Matrix <_Scalar, 6, 6>::Identity();

                /** Form the unknownA matrix **/
                unknownA.template block<6*_RobotTrees, 3> (0,0) = spareI.template block<6*_RobotTrees, 3> (0,0);//Linear Velocities

                /** Form the unknownx **/
                unknownx.template block<3, 1> (0,0) = cartesianVelocities.template block<3, 1> (0,0);

                /** Non-holonomic constraint Slip vector **/
                for (register int i=0; i<_RobotTrees; ++i)
                {
                    unknownA.col(3+i) = -J.col(_RobotJointDoF+(_SlipDoF*(i+1)-1));
                    unknownx[3+i] = modelVelocities[_RobotJointDoF+(_SlipDoF*(i+1)-1)];
                }

                /** Contact angles per each Tree **/
                for (register int i=0; i<(_RobotTrees*_ContactDoF); ++i)
                {
                    unknownA.col(3+_RobotTrees+i) = -J.col(_RobotJointDoF+(_RobotTrees*_SlipDoF)+i);
                    unknownx[3+_RobotTrees+i] = modelVelocities[_RobotJointDoF+(_RobotTrees*_SlipDoF)+i];
                }

                /** Form the knownB matrix **/
                knownB.template block< (6* _RobotTrees), 3> (0,0) = -spareI.template block<6*_RobotTrees, 3> (0,3); //Angular velocities
                knownB.template block< (6* _RobotTrees), _RobotJointDoF> (0,3) = J.template block<6*_RobotTrees, _RobotJointDoF> (0,0);

                /** Form the knowny **/
                knowny.template block<3, 1> (0,0) = cartesianVelocities.template block<3, 1> (3,0);
                knowny.template block<_RobotJointDoF, 1> (3,0) = modelVelocities.template block<_RobotJointDoF, 1> (0,0);

                /** Compute the Weighting matrix: **/
                /** TO-DO: It is based on the sensors noise cov matrices. I also could inlcude the attitude weigthing **/
                /** Form the noise of the know quantities: robot joint encoders + IMU ang. velo (3-axes) **/
                //Eigen::Matrix <_Scalar, 3+_RobotJointDoF, 3+_RobotJointDoF> noiseQ;

                /** NOTE: the first element in the vector/matrix of known quantities are the joint values (encoders). Have a look to MODEL_DOF **/
                //noiseQ.template block < _RobotJointDoF, _RobotJointDoF > (0,0) = modelVelCov.template block< _RobotJointDoF, _RobotJointDoF > (0,0);

                /** The inertial rotation from gyroscopes. The cartesian velocities are stored in the order: first linear and then angular **/
                //noiseQ.bottomRightCorner<3,3>() = cartesianVelCov.bottomRightCorner<3,3>();

                //Weight = (knownB * noiseQ.inverse() * knownB.transpose()).inverse();
                //Weight = 0.5 * (Weight + Weight.transpose());

                /** For the time being set it to a constant Identity matrix **/
                Weight.setIdentity();

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<< "[MOTION_MODEL] spareI is of size "<<spareI.rows()<<"x"<<spareI.cols()<<"\n";
                std::cout<< "[MOTION_MODEL] The spareI matrix:\n" << spareI << std::endl;
                std::cout<< "[MOTION_MODEL] unknownA is of size "<<unknownA.rows()<<"x"<<unknownA.cols()<<"\n";
                std::cout<< "[MOTION_MODEL] The unknownA matrix:\n" << unknownA << std::endl;
                std::cout<< "[MOTION_MODEL] knownB is of size "<<knownB.rows()<<"x"<<knownB.cols()<<"\n";
                std::cout<< "[MOTION_MODEL] The knownB matrix:\n" << knownB << std::endl;
                std::cout<< "[MOTION_MODEL] Weight is of size "<<Weight.rows()<<"x"<<Weight.cols()<<"\n";
                std::cout<< "[MOTION_MODEL] The Weight matrix:\n" << Weight << std::endl;
                #endif

                return;
            }

        public:

            /**@brief Default constructor.
             */
            MotionModel()
            {
            }

            /**@brief Constructor
             *
             * Constructs the Motion Model object with the parameters.
             *
             * @param[out] status, true if everything went well.
             * @param[in] method of the points in contact selection algorithm
             * @param[in] pointer to teh object with the Kinematic Model of the robot.
             *
             */
            MotionModel(bool &status, MotionModel::methodContactPoint method,  kinematics_ptr robotModel)
            {
                std::vector<unsigned int> numberContactPoints (_RobotTrees, 0);

                /** Size the contactPoints variable **/
                contactPoints.resize (_RobotTrees);

                /** Assign the robot model **/
                this->robotModel = robotModel;
                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<<"[MOTION_MODEL] Constructor\n";
                #endif

                /** Know the number of contact points per tree **/
                robotModel->contactPointsPerTree(numberContactPoints);

                /** Assign the type of method to select the contact points in contact **/
                this->contactSelection = method;

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<<"[MOTION_MODEL] Get number of trees "<<robotModel->getNumberOfTrees()<<"\n";
                #endif

                if ((robotModel->getNumberOfTrees() == static_cast<int>(numberContactPoints.size())) &&(numberContactPoints.size() == _RobotTrees))
                {
                    /** Fill the internal contact point variable **/
                    for (register unsigned int i=0; i<numberContactPoints.size(); ++i)
                    {
                        contactPoints[i].number = numberContactPoints[i]; //!Number of contact points
                        contactPoints[i].contactId = 0; //! By default set the contact point zero.
                    }

                    /** Print information message **/
                    LOG_INFO("[MOTION_MODEL] Created Motion Model using %s Robot Kinematics implementation\n", this->robotModel->name().c_str());
                    LOG_INFO("[MOTION_MODEL] %s has %d independent kinematic trees.\n", this->robotModel->name().c_str(), this->robotModel->getNumberOfTrees());
                    status = true;
                }
                else
                {
                    LOG_ERROR("[MOTION_MODEL] Malfunction of the MotionModel. WRONG number of template parameters\n");
                    status = false;
                }

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<<"[MOTION_MODEL] contactPoints contains: ";
                for (std::vector<TreeContactPoint>::iterator it = contactPoints.begin() ; it != contactPoints.end(); it++)
                {
                    std::cout<<" number: "<<(*it).number<<" contactId: "<<(*it).contactId;
                }

                std::cout<<"\n[MOTION_MODEL] **** END ****\n";
                #endif

                return;
            }

            /**@brief Default destructor
             */
            ~MotionModel()
            {
            }

            /**@brief Updates the Kinematic Model of the robot.
             *
             * It computs the Forward Kienmatics of the robot and update the points in contacts.
             *
             * @param[in] modelPositions, the vector with the model positions (joints, slip, contact angle).
             */
            void updateKinematics (const Eigen::Matrix <_Scalar, MODEL_DOF, 1> &modelPositions)
            {
                register int i=0;
                std::vector<_Scalar> vectorPositions(MODEL_DOF, 0);
                std::vector<int> contactId (_RobotTrees, 0);

                /** Copy Eigen to vector **/
                Eigen::Map <Eigen::Matrix <_Scalar, MODEL_DOF, 1> > (&(vectorPositions[0]), MODEL_DOF) = modelPositions;

                /** Solve the forward kinematics for the complete Robot (all the chains)**/
                robotModel->fkSolver(vectorPositions, fkRobot, fkCov);

                /** Select the contact point **/
                this->selectPointsInContact (this->contactSelection);

                /** Set the contact point to the kinematic model **/
                for (std::vector<TreeContactPoint>::iterator it = contactPoints.begin() ; it != contactPoints.end(); it++)
                {
                    contactId[i] = (*it).contactId;
                    i++;
                }
                robotModel->setPointsInContact(contactId);

                return;
            }


            /**@brief Returns the kinematic of the robot
             *
             * @param[out] robot forward kinematics as a vector of affine transformations (one per Tree)
             * @param[out] covariance of the trasnformation.
             */
            inline virtual void getKinematics (std::vector<Eigen::Affine3d> &currentFkRobot, std::vector<base::Matrix6d> &currentFkCov)
            {
                currentFkRobot = this->fkRobot;
                currentFkCov = this->fkCov;

                return;
            }

            /**@brief Return the current points in contact
             *
             * @return vector of points in contact for the robot (point per Tree)
             * in teh same order that the Trees are specify.
             */
            virtual std::vector< int > getPointsInContact ()
            {
                register int i=0;
                std::vector<int> contactId (_RobotTrees, 0);

                for (std::vector<TreeContactPoint>::iterator it = contactPoints.begin() ; it != contactPoints.end(); it++)
                {
                    contactId[i] = (*it).contactId;
                    i++;
                }

                return contactId;
            }


            /**@bief Solver for the Navigation equations
             *
             * This method get the robot Jacoian matrix and computes the Navigation
             * kinematics equations to finally solve the Weighting Least-Square method.
             *
             * The Navigation kinematics euation form A * x = B * y
             * This method(navigation solver) perform the solution to the system above, solving:
             *
             *               x = (A^T * W * A)^(-1) * A^T * W * B * y
             *
             * @param[in] modelPositions, position values of the model (joints, slip and contact angle)
             * @param[in, out] cartesianVelocities, velocities of the robot cartesian space (w.r.t local body frame).
             * @param[in] modelVelocities, velocity values of the model 9joints, slip and contact angle)
             * @param[in] cartesianVelCov, uncertainty in the measurement of the robot (local body frame) cartesian velocities.
             * @param[in] modelVelCov, uncertainty in the meassurement of the robot model velocities (joint space).
             */
            virtual double navSolver(const Eigen::Matrix <_Scalar, MODEL_DOF, 1> &modelPositions,
                                            Eigen::Matrix <_Scalar, 6, 1> &cartesianVelocities,
                                            Eigen::Matrix <_Scalar, MODEL_DOF, 1> &modelVelocities,
                                            Eigen::Matrix <_Scalar, 6, 6> &cartesianVelCov,
                                            Eigen::Matrix <_Scalar, MODEL_DOF, MODEL_DOF> &modelVelCov)
            {
                double normalizedError = std::numeric_limits<double>::quiet_NaN(); //solution error of the Least-Squares
                std::vector<_Scalar> vectorPositions(MODEL_DOF, 0); // model positions in std_vector form
                Eigen::Matrix <_Scalar, 6*_RobotTrees, MODEL_DOF> J; // Robot Jacobian
                Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotTrees+(_RobotTrees*_ContactDoF)> unknownA; // Nav non-sensed values matrix
                Eigen::Matrix <_Scalar, 3+_RobotTrees+(_RobotTrees*_ContactDoF), 1> unknownx; // Nav non-sensed values vector
                Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotJointDoF> knownB; // Nav sensed values matrix
                Eigen::Matrix <_Scalar, 3+_RobotJointDoF, 1> knowny; // Nav sensed values vector
                Eigen::Matrix <_Scalar, 6*_RobotTrees, 6*_RobotTrees> Weight; // (Sensor noise)^(-1) and Trees Weighting matrix (which robot's tree has more weight to the computation).

                /** Initialize  variables **/
                unknownA.setZero(); unknownx.setZero();
                knownB.setZero(); knowny.setZero();
                Weight.setZero();

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout << "[MOTION_MODEL] cartesianVelocities is of size "<<cartesianVelocities.rows()<<"x"<<cartesianVelocities.cols()<<"\n";
                std::cout << "[MOTION_MODEL] cartesianVelocities is \n" << cartesianVelocities<< std::endl;

                std::cout << "[MOTION_MODEL] modelVelocities is of size "<<modelVelocities.rows()<<"x"<<modelVelocities.cols()<<"\n";
                std::cout << "[MOTION_MODEL] modelVelocities is \n" << modelVelocities<< std::endl;
                #endif

                /** Copy Eigen to vector **/
                Eigen::Map <Eigen::Matrix <_Scalar, MODEL_DOF, 1> > (&(vectorPositions[0]), MODEL_DOF) = modelPositions;

                /** Solve the Robot Jacobian Matrix **/
                J = robotModel->jacobianSolver (vectorPositions);

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout<< "[MOTION_MODEL] J is of size "<<J.rows()<<"x"<<J.cols()<<"\n";
                std::cout<< "[MOTION_MODEL] The J matrix \n" << J << std::endl;
                #endif

                /** Form the Composite Navigation Equations and Noise Covariance **/
                this->navEquations (cartesianVelocities, modelVelocities, J,
                        cartesianVelCov, modelVelCov, unknownA, unknownx, knownB, knowny, Weight);

                /** Solve the Motion Model by Least-Squares (navigation kinematics) **/
                Eigen::Matrix <_Scalar,3+_RobotTrees+(_RobotTrees*_ContactDoF), 3+_RobotTrees+(_RobotTrees*_ContactDoF)> pseudoInvUnknownA;
                Eigen::Matrix <_Scalar, 6*_RobotTrees, 1> knownb = knownB*knowny;

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                /** DEBUG OUTPUT **/
                typedef Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotTrees+(_RobotTrees*_ContactDoF)> matrixAType;
                typedef Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotJointDoF> matrixBType;
                typedef Eigen::Matrix <_Scalar, 6*_RobotTrees, 3+_RobotTrees+(_RobotTrees*_ContactDoF)+1> matrixConjType; // columns are columns of A + 1

                matrixConjType Conj;

                Eigen::FullPivLU<matrixAType> lu_decompA(unknownA);
                std::cout << "[MOTION_MODEL] The rank of A is " << lu_decompA.rank() << std::endl;

                Eigen::FullPivLU<matrixBType> lu_decompB(knownB);
                std::cout << "[MOTION_MODEL] The rank of B is " << lu_decompB.rank() << std::endl;

                Conj.template block<6*_RobotTrees,3+_RobotTrees+(_RobotTrees*_ContactDoF)>(0,0) = unknownA;
                Conj.template block<6*_RobotTrees, 1>(0,3+_RobotTrees+(_RobotTrees*_ContactDoF)) = knownb;
                Eigen::FullPivLU<matrixConjType> lu_decompConj(Conj);
                std::cout << "[MOTION_MODEL] The rank of A|B*y is " << lu_decompConj.rank() << std::endl;
                std::cout << "[MOTION_MODEL] Pseudoinverse of A\n" << (unknownA.transpose() * Weight * unknownA).inverse() << std::endl;
                /*******************/
                #endif

                pseudoInvUnknownA = (unknownA.transpose() * Weight * unknownA).inverse();
                unknownx = pseudoInvUnknownA * unknownA.transpose() * Weight * knownb;

                /** Error of the solution **/
                Eigen::Matrix<double, 1,1> squaredError = (((unknownA*unknownx - knownb).transpose() * Weight * (unknownA*unknownx - knownb)));
                if (knownb.norm() != 0.00)
                    normalizedError = sqrt(squaredError[0]) / knownb.norm();

                /** Save the results in the parameters (NaN previous values are now known quantities) **/
                cartesianVelocities.template block<3, 1>(0,0) = unknownx.template block<3, 1>(0,0); // Linear velocities
                cartesianVelCov.template block<3, 3> (0,0) = squaredError[0] * Eigen::Matrix<_Scalar, 3, 3>::Identity();//pseudoInvUnknownA.template block<3, 3> (0,0);//Linear Velocities noise

                /** Non-holonomic constraint Slip vector **/
                for (register int i=0; i<_RobotTrees; ++i)
                {
                    modelVelocities[_RobotJointDoF+(_SlipDoF*(i+1)-1)] = unknownx[3+i];
                    modelVelCov.col(_RobotJointDoF+(_SlipDoF*(i+1)-1))[_RobotJointDoF+(_SlipDoF*(i+1)-1)] = squaredError[0];//pseudoInvUnknownA.col(3+i)[3+i]; For the time being set the error to the error in the estimation
                }

                /** Contact angles per each Tree **/
                for (register int i=0; i<(_RobotTrees*_ContactDoF); ++i)
                {
                    modelVelocities[_RobotJointDoF+(_RobotTrees*_SlipDoF)+i] = unknownx[3+_RobotTrees+i];
                    modelVelCov.col(_RobotJointDoF+(_RobotTrees*_SlipDoF)+i)[_RobotJointDoF+(_RobotTrees*_SlipDoF)+i] = squaredError[0];//pseudoInvUnknownA.col(3+_RobotTrees+i)[3+_RobotTrees+i];
                }

                #ifdef DEBUG_PRINTS_ODOMETRY_MOTION_MODEL
                std::cout << "[MOTION_MODEL] L-S solution:\n"<<unknownx<<std::endl;

                std::cout << "[MOTION_MODEL] cartesianVelocities is of size "<<cartesianVelocities.rows()<<"x"<<cartesianVelocities.cols()<<"\n";
                std::cout << "[MOTION_MODEL] cartesianVelocities is \n" << cartesianVelocities<< std::endl;

                std::cout << "[MOTION_MODEL] cartesianVelCov is of size "<<cartesianVelCov.rows()<<"x"<<cartesianVelCov.cols()<<"\n";
                std::cout << "[MOTION_MODEL] cartesianVelCov is \n" << cartesianVelCov<< std::endl;

                std::cout << "[MOTION_MODEL] modelVelocities is of size "<<modelVelocities.rows()<<"x"<<modelVelocities.cols()<<"\n";
                std::cout << "[MOTION_MODEL] modelVelocities is \n" << modelVelocities<< std::endl;

                std::cout << "[MOTION_MODEL] modelVelCov is of size "<<modelVelCov.rows()<<"x"<<modelVelCov.cols()<<"\n";
                std::cout << "[MOTION_MODEL] modelVelCov is \n" << modelVelCov<< std::endl;

                std::cout << "[MOTION_MODEL] The absolute least squared error is:\n" << squaredError << std::endl;
                std::cout << "[MOTION_MODEL] The relative error is:\n" << normalizedError << std::endl;
                #endif


                return normalizedError;
            }

            /**@brief Solver for the Slip equations
             * TO-DO
             */
            virtual base::Vector6d slipSolver(void)
            {
                base::Vector6d s;

                return s;
            }
    };
}

#endif //ODOMETRY_MOTION_MODEL_HPP
